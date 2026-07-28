#ifndef slic3r_GCodeViewer_hpp_
#define slic3r_GCodeViewer_hpp_

#include "3DScene.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "IMSlider.hpp"
#include "GLModel.hpp"
#include "I18N.hpp"

#include <boost/iostreams/device/mapped_file.hpp>
#include <nlohmann/json.hpp> // NEOTKO_GCODE_REPROCESSOR — ExpertReprocessorPanelState::unknown_rules

#include <cstdint>
#include <float.h>
#include <set>
#include <unordered_set>

namespace Slic3r {

class Print;
class TriangleMesh;
class PresetBundle;

namespace GUI {

class PartPlateList;
class OpenGLManager;
class Camera;

static const float GCODE_VIEWER_SLIDER_SCALE = 0.6f;
static const float SLIDER_DEFAULT_RIGHT_MARGIN  = 10.0f;
static const float SLIDER_DEFAULT_BOTTOM_MARGIN = 10.0f;
static const float SLIDER_RIGHT_MARGIN = 124.0f;
static const float SLIDER_BOTTOM_MARGIN = 64.0f;
class GCodeViewer
{
    using IBufferType = unsigned short;
    using VertexBuffer = std::vector<float>;
    using MultiVertexBuffer = std::vector<VertexBuffer>;
    using IndexBuffer = std::vector<IBufferType>;
    using MultiIndexBuffer = std::vector<IndexBuffer>;
    using InstanceBuffer = std::vector<float>;
    using InstanceIdBuffer = std::vector<size_t>;
    using InstancesOffsets = std::vector<Vec3f>;

    static const std::vector<ColorRGBA> Extrusion_Role_Colors;
    static const std::vector<ColorRGBA> Options_Colors;
    static const std::vector<ColorRGBA> Travel_Colors;
    static const std::vector<ColorRGBA> Range_Colors;
    static const ColorRGBA              Wipe_Color;
    static const ColorRGBA              Neutral_Color;

    enum class EOptionsColors : unsigned char
    {
        Retractions,
        Unretractions,
        Seams,
        ToolChanges,
        ColorChanges,
        PausePrints,
        CustomGCodes
    };

    // vbo buffer containing vertices data used to render a specific toolpath type
    struct VBuffer
    {
        enum class EFormat : unsigned char
        {
            // vertex format: 3 floats -> position.x|position.y|position.z
            Position,
            // vertex format: 4 floats -> position.x|position.y|position.z|normal.x
            PositionNormal1,
            // vertex format: 6 floats -> position.x|position.y|position.z|normal.x|normal.y|normal.z
            PositionNormal3
        };

        EFormat format{ EFormat::Position };
        // vbos id
        std::vector<unsigned int> vbos;
        // sizes of the buffers, in bytes, used in export to obj
        std::vector<size_t> sizes;
        // count of vertices, updated after data are sent to gpu
        size_t count{ 0 };

        size_t data_size_bytes() const { return count * vertex_size_bytes(); }
        // We set 65536 as max count of vertices inside a vertex buffer to allow
        // to use unsigned short in place of unsigned int for indices in the index buffer, to save memory
        size_t max_size_bytes() const { return 65536 * vertex_size_bytes(); }

        size_t vertex_size_floats() const { return position_size_floats() + normal_size_floats(); }
        size_t vertex_size_bytes() const { return vertex_size_floats() * sizeof(float); }

        size_t position_offset_floats() const { return 0; }
        size_t position_offset_bytes() const { return position_offset_floats() * sizeof(float); }

        size_t position_size_floats() const { return 3; }
        size_t position_size_bytes() const { return position_size_floats() * sizeof(float); }

        size_t normal_offset_floats() const {
            assert(format == EFormat::PositionNormal1 || format == EFormat::PositionNormal3);
            return position_size_floats();
        }
        size_t normal_offset_bytes() const { return normal_offset_floats() * sizeof(float); }

        size_t normal_size_floats() const {
            switch (format)
            {
            case EFormat::PositionNormal1: { return 1; }
            case EFormat::PositionNormal3: { return 3; }
            default:                       { return 0; }
            }
        }
        size_t normal_size_bytes() const { return normal_size_floats() * sizeof(float); }

        void reset();
    };

    // buffer containing instances data used to render a toolpaths using instanced or batched models
    // instance record format:
    // instanced models: 5 floats -> position.x|position.y|position.z|width|height (which are sent to the shader as -> vec3 (offset) + vec2 (scales) in GLModel::render_instanced())
    // batched models:   3 floats -> position.x|position.y|position.z
    struct InstanceVBuffer
    {
        // ranges used to render only subparts of the intances
        struct Ranges
        {
            struct Range
            {
                // offset in bytes of the 1st instance to render
                unsigned int offset;
                // count of instances to render
                unsigned int count;
                // vbo id
                unsigned int vbo{ 0 };
                // Color to apply to the instances
                ColorRGBA color;
            };

            std::vector<Range> ranges;

            void reset();
        };

        enum class EFormat : unsigned char
        {
            InstancedModel,
            BatchedModel
        };

        EFormat format;

        // cpu-side buffer containing all instances data
        InstanceBuffer buffer;
        // indices of the moves for all instances
        std::vector<size_t> s_ids;
        // position offsets, used to show the correct value of the tool position
        InstancesOffsets offsets;
        Ranges render_ranges;

        size_t data_size_bytes() const { return s_ids.size() * instance_size_bytes(); }

        size_t instance_size_floats() const {
            switch (format)
            {
            case EFormat::InstancedModel: { return 5; }
            case EFormat::BatchedModel: { return 3; }
            default: { return 0; }
            }
        }
        size_t instance_size_bytes() const { return instance_size_floats() * sizeof(float); }

        void reset();
    };

    // ibo buffer containing indices data (for lines/triangles) used to render a specific toolpath type
    struct IBuffer
    {
        // id of the associated vertex buffer
        unsigned int vbo{ 0 };
        // ibo id
        unsigned int ibo{ 0 };
        // count of indices, updated after data are sent to gpu
        size_t count{ 0 };

        void reset();
    };

    // Used to identify different toolpath sub-types inside a IBuffer
    struct Path
    {
        struct Endpoint
        {
            // index of the buffer in the multibuffer vector
            // the buffer type may change:
            // it is the vertex buffer while extracting vertices data,
            // the index buffer while extracting indices data
            unsigned int b_id{ 0 };
            // index into the buffer
            size_t i_id{ 0 };
            // move id
            size_t s_id{ 0 };
            Vec3f position{ Vec3f::Zero() };
        };

        struct Sub_Path
        {
            Endpoint first;
            Endpoint last;

            bool contains(size_t s_id) const {
                return first.s_id <= s_id && s_id <= last.s_id;
            }
        };

        EMoveType type{ EMoveType::Noop };
        ExtrusionRole role{ erNone };
        float delta_extruder{ 0.0f };
        float height{ 0.0f };
        float width{ 0.0f };
        float feedrate{ 0.0f };
        float fan_speed{ 0.0f };
        float temperature{ 0.0f };
        float volumetric_rate{ 0.0f };
        float layer_time{ 0.0f };
        unsigned char extruder_id{ 0 };
        unsigned char cp_color_id{ 0 };
        std::vector<Sub_Path> sub_paths;

        bool matches(const GCodeProcessorResult::MoveVertex& move) const;
        size_t vertices_count() const {
            return sub_paths.empty() ? 0 : sub_paths.back().last.s_id - sub_paths.front().first.s_id + 1;
        }
        bool contains(size_t s_id) const {
            return sub_paths.empty() ? false : sub_paths.front().first.s_id <= s_id && s_id <= sub_paths.back().last.s_id;
        }
        int get_id_of_sub_path_containing(size_t s_id) const {
            if (sub_paths.empty())
                return -1;
            else {
                for (int i = 0; i < static_cast<int>(sub_paths.size()); ++i) {
                    if (sub_paths[i].contains(s_id))
                        return i;
                }
                return -1;
            }
        }
        void add_sub_path(const GCodeProcessorResult::MoveVertex& move, unsigned int b_id, size_t i_id, size_t s_id) {
            Endpoint endpoint = { b_id, i_id, s_id, move.position };
            sub_paths.push_back({ endpoint , endpoint });
        }
    };

    // Used to batch the indices needed to render the paths
    struct RenderPath
    {
        // Index of the parent tbuffer
        unsigned char               tbuffer_id;
        // Render path property
        ColorRGBA                       color;
        // Index of the buffer in TBuffer::indices
        unsigned int                ibuffer_id;
        // Render path content
        // Index of the path in TBuffer::paths
        unsigned int                path_id;
        std::vector<unsigned int>   sizes;
        std::vector<size_t>         offsets; // use size_t because we need an unsigned integer whose size matches pointer's size (used in the call glMultiDrawElements())
        // NEOTKO_REALCOLOR_TAG: parallel to sizes/offsets — the REAL originating Path index for
        // each sub-draw (path_id above is only the FIRST Path of the color-batch and is wrong
        // for any sub-draw after it when paths sharing a color have different heights, e.g. a
        // PathBlend ramp). Only consumed by GCodeViewer::render_toolpaths_realcolor(); every
        // other renderer keeps using path_id as before.
        std::vector<unsigned int>   path_ids;
        bool contains(size_t offset) const {
            for (size_t i = 0; i < offsets.size(); ++i) {
                if (offsets[i] <= offset && offset <= offsets[i] + static_cast<size_t>(sizes[i] * sizeof(IBufferType)))
                    return true;
            }
            return false;
        }
    };
    struct RenderPathPropertyLower {
        bool operator() (const RenderPath &l, const RenderPath &r) const {
            if (l.tbuffer_id < r.tbuffer_id)
                return true;
            if (l.color < r.color)
                return true;
            else if (l.color > r.color)
                return false;
            return l.ibuffer_id < r.ibuffer_id;
        }
    };
    struct RenderPathPropertyEqual {
        bool operator() (const RenderPath &l, const RenderPath &r) const {
            return l.tbuffer_id == r.tbuffer_id && l.ibuffer_id == r.ibuffer_id && l.color == r.color;
        }
    };

    // buffer containing data for rendering a specific toolpath type
    struct TBuffer
    {
        enum class ERenderPrimitiveType : unsigned char
        {
            Line,
            Triangle,
            InstancedModel,
            BatchedModel
        };

        ERenderPrimitiveType render_primitive_type;

        // buffers for point, line and triangle primitive types
        VBuffer vertices;
        std::vector<IBuffer> indices;

        struct Model
        {
            GLModel model;
            ColorRGBA color;
            InstanceVBuffer instances;
            GLModel::Geometry data;

            void reset();
        };

        // contain the buffer for model primitive types
        Model model;

        std::string shader;
        std::vector<Path> paths;
        std::vector<RenderPath> render_paths;
        bool visible{ false };

        void reset();

        // b_id index of buffer contained in this->indices
        // i_id index of first index contained in this->indices[b_id]
        // s_id index of first vertex contained in this->vertices
        void add_path(const GCodeProcessorResult::MoveVertex& move, unsigned int b_id, size_t i_id, size_t s_id);

        unsigned int max_vertices_per_segment() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:     { return 2; }
            case ERenderPrimitiveType::Triangle: { return 8; }
            default:                             { return 0; }
            }
        }

        size_t max_vertices_per_segment_size_floats() const { return vertices.vertex_size_floats() * static_cast<size_t>(max_vertices_per_segment()); }
        size_t max_vertices_per_segment_size_bytes() const { return max_vertices_per_segment_size_floats() * sizeof(float); }
        unsigned int indices_per_segment() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:     { return 2; }
            case ERenderPrimitiveType::Triangle: { return 30; } // 3 indices x 10 triangles
            default:                             { return 0; }
            }
        }
        size_t indices_per_segment_size_bytes() const { return static_cast<size_t>(indices_per_segment() * sizeof(IBufferType)); }
        unsigned int max_indices_per_segment() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:     { return 2; }
            case ERenderPrimitiveType::Triangle: { return 36; } // 3 indices x 12 triangles
            default:                             { return 0; }
            }
        }
        size_t max_indices_per_segment_size_bytes() const { return max_indices_per_segment() * sizeof(IBufferType); }

        bool has_data() const {
            switch (render_primitive_type)
            {
            case ERenderPrimitiveType::Line:
            case ERenderPrimitiveType::Triangle: {
                return !vertices.vbos.empty() && vertices.vbos.front() != 0 && !indices.empty() && indices.front().ibo != 0;
            }
            case ERenderPrimitiveType::InstancedModel: { return model.model.is_initialized() && !model.instances.buffer.empty(); }
            case ERenderPrimitiveType::BatchedModel: {
                return !model.data.vertices.empty() && !model.data.indices.empty() &&
                    !vertices.vbos.empty() && vertices.vbos.front() != 0 && !indices.empty() && indices.front().ibo != 0;
            }
            default: { return false; }
            }
        }
    };

    // helper to render shells
    struct Shells
    {
        GLVolumeCollection volumes;
        bool visible{ false };
        //BBS: always load shell when preview
        int print_id{ -1 };
        int print_modify_count { -1 };
        bool previewing{ false };
    };

    // helper to render extrusion paths
    struct Extrusions
    {
        struct Range
        {
            float min;
            float max;
            unsigned int count;
            bool log_scale;

            Range() { reset(); }
            void update_from(const float value) {
                if (value != max && value != min)
                    ++count;
                min = std::min(min, value);
                max = std::max(max, value);
            }
            void reset(bool log = false) { min = FLT_MAX; max = -FLT_MAX; count = 0; log_scale = log; }

            float step_size() const;
            ColorRGBA get_color_at(float value) const;
            float get_value_at_step(int step) const;

        };

        struct Ranges
        {
            // Color mapping by layer height.
            Range height;
            // Color mapping by extrusion width.
            Range width;
            // Color mapping by feedrate.
            Range feedrate;
            // Color mapping by fan speed.
            Range fan_speed;
            // Color mapping by volumetric extrusion rate.
            Range volumetric_rate;
            // Color mapping by extrusion temperature.
            Range temperature;
            // Color mapping by layer time.
            Range layer_duration;
Range layer_duration_log;
            void reset() {
                height.reset();
                width.reset();
                feedrate.reset();
                fan_speed.reset();
                volumetric_rate.reset();
                temperature.reset();
                layer_duration.reset();
                layer_duration_log.reset(true);
            }
        };

        unsigned int role_visibility_flags{ 0 };
        Ranges ranges;

        void reset_role_visibility_flags() {
            role_visibility_flags = 0;
            for (unsigned int i = 0; i < erCount; ++i) {
                role_visibility_flags |= 1 << i;
            }
        }

        void reset_ranges() { ranges.reset(); }
    };

    class Layers
    {
    public:
        struct Endpoints
        {
            size_t first{ 0 };
            size_t last{ 0 };

            bool operator == (const Endpoints& other) const { return first == other.first && last == other.last; }
            bool operator != (const Endpoints& other) const { return !operator==(other); }
        };

    private:
        std::vector<double> m_zs;
        std::vector<Endpoints> m_endpoints;

    public:
        void append(double z, Endpoints endpoints) {
            m_zs.emplace_back(z);
            m_endpoints.emplace_back(endpoints);
        }

        void reset() {
            m_zs = std::vector<double>();
            m_endpoints = std::vector<Endpoints>();
        }

        size_t size() const { return m_zs.size(); }
        bool empty() const { return m_zs.empty(); }
        const std::vector<double>& get_zs() const { return m_zs; }
        const std::vector<Endpoints>& get_endpoints() const { return m_endpoints; }
        std::vector<Endpoints>& get_endpoints() { return m_endpoints; }
        double get_z_at(unsigned int id) const { return (id < m_zs.size()) ? m_zs[id] : 0.0; }
        Endpoints get_endpoints_at(unsigned int id) const { return (id < m_endpoints.size()) ? m_endpoints[id] : Endpoints(); }
        int                           get_l_at(float z) const
        {
            auto iter = std::upper_bound(m_zs.begin(), m_zs.end(), z);
            return std::distance(m_zs.begin(), iter);
        }

        bool operator != (const Layers& other) const {
            if (m_zs != other.m_zs)
                return true;
            if (m_endpoints != other.m_endpoints)
                return true;
            return false;
        }
    };

    // used to render the toolpath caps of the current sequential range
    // (i.e. when sliding on the horizontal slider)
    struct SequentialRangeCap
    {
        TBuffer* buffer{ nullptr };
        unsigned int ibo{ 0 };
        unsigned int vbo{ 0 };
        ColorRGBA color;

        ~SequentialRangeCap();
        bool is_renderable() const { return buffer != nullptr; }
        void reset();
        size_t indices_count() const { return 6; }
    };

#if ENABLE_GCODE_VIEWER_STATISTICS
    struct Statistics
    {
        // time
        int64_t results_time{ 0 };
        int64_t load_time{ 0 };
        int64_t load_vertices{ 0 };
        int64_t smooth_vertices{ 0 };
        int64_t load_indices{ 0 };
        int64_t refresh_time{ 0 };
        int64_t refresh_paths_time{ 0 };
        // opengl calls
        int64_t gl_multi_lines_calls_count{ 0 };
        int64_t gl_multi_triangles_calls_count{ 0 };
        int64_t gl_triangles_calls_count{ 0 };
        int64_t gl_instanced_models_calls_count{ 0 };
        int64_t gl_batched_models_calls_count{ 0 };
        // memory
        int64_t results_size{ 0 };
        int64_t total_vertices_gpu_size{ 0 };
        int64_t total_indices_gpu_size{ 0 };
        int64_t total_instances_gpu_size{ 0 };
        int64_t max_vbuffer_gpu_size{ 0 };
        int64_t max_ibuffer_gpu_size{ 0 };
        int64_t paths_size{ 0 };
        int64_t render_paths_size{ 0 };
        int64_t models_instances_size{ 0 };
        // other
        int64_t travel_segments_count{ 0 };
        int64_t wipe_segments_count{ 0 };
        int64_t extrude_segments_count{ 0 };
        int64_t instances_count{ 0 };
        int64_t batched_count{ 0 };
        int64_t vbuffers_count{ 0 };
        int64_t ibuffers_count{ 0 };

        void reset_all() {
            reset_times();
            reset_opengl();
            reset_sizes();
            reset_others();
        }

        void reset_times() {
            results_time = 0;
            load_time = 0;
            load_vertices = 0;
            smooth_vertices = 0;
            load_indices = 0;
            refresh_time = 0;
            refresh_paths_time = 0;
        }

        void reset_opengl() {
            gl_multi_lines_calls_count = 0;
            gl_multi_triangles_calls_count = 0;
            gl_triangles_calls_count = 0;
            gl_instanced_models_calls_count = 0;
            gl_batched_models_calls_count = 0;
        }

        void reset_sizes() {
            results_size = 0;
            total_vertices_gpu_size = 0;
            total_indices_gpu_size = 0;
            total_instances_gpu_size = 0;
            max_vbuffer_gpu_size = 0;
            max_ibuffer_gpu_size = 0;
            paths_size = 0;
            render_paths_size = 0;
            models_instances_size = 0;
        }

        void reset_others() {
            travel_segments_count = 0;
            wipe_segments_count = 0;
            extrude_segments_count = 0;
            instances_count = 0;
            batched_count = 0;
            vbuffers_count = 0;
            ibuffers_count = 0;
        }
    };
#endif // ENABLE_GCODE_VIEWER_STATISTICS

public:
    enum class EViewType : unsigned char;
    struct SequentialView
    {
        class Marker
        {
            GLModel m_model;
            Vec3f m_world_position;
            Transform3f m_world_transform;
            // for seams, the position of the marker is on the last endpoint of the toolpath containing it
            // the offset is used to show the correct value of tool position in the "ToolPosition" window
            // see implementation of render() method
            Vec3f m_world_offset;
            float m_z_offset{ 0.5f };
            GCodeProcessorResult::MoveVertex m_curr_move;
            bool m_visible{ true };
            bool m_is_dark = false;

        public:
            float m_scale = 1.0f;

            void init(std::string filename);

            const BoundingBoxf3& get_bounding_box() const { return m_model.get_bounding_box(); }

            void set_world_position(const Vec3f& position);
            void set_world_offset(const Vec3f& offset) { m_world_offset = offset; }

            bool is_visible() const { return m_visible; }
            void set_visible(bool visible) { m_visible = visible; }

            void render(int canvas_width, int canvas_height, const EViewType& view_type);
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }

            void update_curr_move(const GCodeProcessorResult::MoveVertex move);
        };

        class GCodeWindow
        {
            struct Line
            {
                std::string command;
                std::string parameters;
                std::string comment;
            };
            bool m_is_dark = false;
            uint64_t m_selected_line_id{ 0 };
            size_t m_last_lines_size{ 0 };
            std::string m_filename;
            boost::iostreams::mapped_file_source m_file;
            // map for accessing data in file by line number
            std::vector<size_t> m_lines_ends;
            // current visible lines
            std::vector<Line> m_lines;

        public:
            float m_scale = 1.0f;
            GCodeWindow() = default;
            ~GCodeWindow() { stop_mapping_file(); }
            void load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends);
            void reset() {
                stop_mapping_file();
                m_lines_ends.clear();
                m_lines_ends.shrink_to_fit();
                m_lines.clear();
                m_lines.shrink_to_fit();
                m_filename.clear();
                m_filename.shrink_to_fit();
            }

            //BBS: GUI refactor: add canvas size
            //void render(float top, float bottom, uint64_t curr_line_id) const;
            void render(float top, float bottom, float right, uint64_t curr_line_id) const;
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }

            void stop_mapping_file();
        };

        struct Endpoints
        {
            size_t first{ 0 };
            size_t last{ 0 };
        };

        bool skip_invisible_moves{ false };
        Endpoints endpoints;
        Endpoints current;
        Endpoints last_current;
        Endpoints global;
        Vec3f current_position{ Vec3f::Zero() };
        Vec3f current_offset{ Vec3f::Zero() };
        Marker marker;
        GCodeWindow gcode_window;
        std::vector<unsigned int> gcode_ids;
        float m_scale = 1.0;
        bool m_show_marker = false;
        void render(const bool has_render_path, float legend_height, int canvas_width, int canvas_height, int right_margin, const EViewType& view_type);
    };

    struct ETools
    {
        std::vector<ColorRGBA> m_tool_colors;
        std::vector<bool>  m_tool_visibles;
    };

    enum class EViewType : unsigned char
    {
        FeatureType = 0,
        Height,
        Width,
        Feedrate,
        FanSpeed,
        Temperature,
        VolumetricRate,
        Tool,
        ColorPrint,
        FilamentId,
        LayerTime,
        LayerTimeLog,
        RealColor,
        // NEOTKO_GCODE_REPROCESSOR (s216): folds the rule editor into the legend's view-type
        // combo (same pattern as RealColor above) instead of a separate floating ImGui window —
        // see render_legend()'s early-return branch and render_expert_gcode_reprocessor_panel().
        GCodeReprocessor,
        Count
    };

    // NEOTKO_REALCOLOR_TAG: LUT of TD/rgb per physical tool for the RealColor view,
    // read from raw filament_colour + app_config neotko_td_1..4 (same source as
    // Sandwich Editor/Painter, see ColorSci.cpp::blend_stacked). NOTE (s166): unlike the
    // Sandwich/Painter previews, `td` here is baked to MILLIMETERS (neotko_td_N in its native
    // ratio-units × nominal layer_height) in refresh_realcolor_materials() — RealColor's peel
    // composites against real physical mm thickness per pass, not a layer-height-relative
    // ratio, so it needs mm-space TD to stay consistent. See the UNIT FIX comment there.
    struct RealColorMaterials
    {
        std::array<ColorRGB, 4> rgb{};
        std::array<float, 4>    td{};
        bool                     valid = false;
    };

    // NEOTKO_REALCOLOR_TAG: fingerprint of everything that can invalidate the depth-peeled
    // composite cache (camera, TD/color, layer/moves slider range, viewport size).
    // Plain field-by-field struct, compared with memcmp — cheaper than string hashing.
    struct RealColorFingerprint
    {
        std::array<float, 16> view{};
        std::array<float, 16> proj{};
        int canvas_w = -1, canvas_h = -1;
        std::array<float, 4> td{};
        std::array<ColorRGB, 4> rgb{};
        unsigned int layers_z_range[2] = { 0, 0 };
        unsigned int moves_first = 0, moves_last = 0;

        bool operator==(const RealColorFingerprint& other) const {
            return canvas_w == other.canvas_w && canvas_h == other.canvas_h &&
                   layers_z_range[0] == other.layers_z_range[0] && layers_z_range[1] == other.layers_z_range[1] &&
                   moves_first == other.moves_first && moves_last == other.moves_last &&
                   td == other.td && rgb == other.rgb && view == other.view && proj == other.proj;
        }
        bool operator!=(const RealColorFingerprint& other) const { return !(*this == other); }
    };

    // NEOTKO_REALCOLOR_TAG: GL scratch state for depth-peeled Beer-Lambert compositing.
    // 2 peel FBOs (ping-pong: color + tool/thickness meta + depth-as-texture) and 2 accum FBOs
    // (ping-pong: running composited color + running per-channel transmittance). No
    // GL_TEXTURE_2D_ARRAY — peels are consumed immediately into the accumulator, never stored
    // N-deep. See docs/WIP/REALCOLOR_VIEW plan / SANDWICH.md sibling docs for the design.
    struct RealColorCache
    {
        int tex_w = 0, tex_h = 0;

        // GL handles stored as plain unsigned int (not GLuint) so this header doesn't need
        // to pull in GL/glew.h — matches IBuffer::vbo/ibo convention elsewhere in this class.
        unsigned int peel_fbo[2] = { 0, 0 };
        unsigned int peel_color_tex[2]  = { 0, 0 }; // GL_RGBA8, attachment 0: lit color of this peel
        unsigned int peel_meta_tex[2]   = { 0, 0 }; // GL_RGBA32F, attachment 1: r=tool_id, g=thickness
        // NEOTKO_REALCOLOR_TAG s166 (item 3): attachment 2, view-space normal packed [0,1] —
        // consumed only by realcolor_present.fs's SSAO kernel, ignored by realcolor_accum.fs.
        unsigned int peel_normal_tex[2] = { 0, 0 }; // GL_RGBA32F, attachment 2: packed view-space normal
        unsigned int peel_depth_tex[2]  = { 0, 0 }; // GL_DEPTH_COMPONENT32F, sampled by the next peel pass

        unsigned int accum_fbo[2] = { 0, 0 };
        unsigned int accum_color_tex[2]    = { 0, 0 }; // GL_RGBA8, running composited color
        unsigned int accum_transmit_tex[2] = { 0, 0 }; // GL_RGBA32F, running per-channel transmittance
        int    accum_write_slot = 0;             // accum_color_tex[accum_write_slot] = latest result

        unsigned int quad_vbo = 0; // fullscreen NDC quad (4 verts, vec2), shared by accum+present

        bool valid = false;
        RealColorFingerprint fingerprint;

        bool gl_objects_created() const { return peel_fbo[0] != 0; }
    };

    // NEOTKO_REALCOLOR_TAG: live-tunable mirrors of constants that would otherwise be baked
    // into GCodeViewer.cpp/shader #defines — debug-only (edited via the ImGui panel gated by
    // NeoDebug::enabled(NeoDebug::REALCOLOR) in render_toolpaths_realcolor()), not a user
    // setting. Defaults re-calibrated s166, live, against a real print AFTER the s166 TD unit
    // fix (see refresh_realcolor_materials) — ambient raised vs. the s164 values (0.18/0.32) to
    // read flatter/brighter, which the user judged easier to eyeball-compare against real
    // filament swatches than the more sculpted two-tone look; accum_seed_gray dropped to 0
    // since post-fix convergence means it rarely shows except at genuinely thin edges.
    struct RealColorTuning
    {
        float accum_seed_gray  = 0.0f;  // linear-space accumulator background, see item 1
        float ambient_ground   = 0.35f; // realcolor_peel.vs two-tone ambient, see item 2
        float ambient_sky      = 0.45f;
        float fresnel_power    = 5.0f;  // realcolor_peel.vs rim term, see item 2
        float fresnel_strength = 0.05f;
        // NEOTKO_REALCOLOR_TAG: global multiplier applied to every m_realcolor_materials.td[t]
        // before it reaches realcolor_accum.fs — manual override + cheap visual TD calibration
        // (no real TD meter available), see render_realcolor_debug_panel().
        float td_scale         = 1.0f;

        // NEOTKO_REALCOLOR_TAG s214 (PBR item 1, docs/WIP/REALCOLOR_VIEW/08_PBR_IBL_SSS_PLAN.md):
        // single-probe analytic IBL tints for realcolor_peel.{vs,fs} — additive on top of item
        // 2's shipped achromatic ambient/rim, not a replacement. (1,1,1) on any of the three
        // collapses back to the exact s166 behaviour. Defaults: a subtle cool-sky/warm-ground
        // split, picked so the look doesn't jump hard on first compile — retune live like every
        // other slider here.
        std::array<float, 3> ambient_ground_tint = { 1.00f, 0.96f, 0.90f };
        std::array<float, 3> ambient_sky_tint    = { 0.90f, 0.95f, 1.00f };
        std::array<float, 3> fresnel_tint        = { 0.85f, 0.92f, 1.00f };

        // NEOTKO_REALCOLOR_TAG s214 (PBR item 2): single-pass screen-space subsurface scattering
        // in realcolor_present.fs — see compute_sss() there. sss_strength=0 is bit-identical to
        // pre-s214 output.
        float sss_strength     = 0.35f;
        float sss_radius_px    = 6.0f;
        float sss_reference_td = 1.0f;
    };

    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3, docs/WIP/REALCOLOR_VIEW/09_HDR_ENVIRONMENT_PLAN.md):
    // procedural equirectangular "studio" environment — two flat baked GL_RGBA8 textures, no
    // mipmaps (see 09's rationale for why GLTexture::load_from_raw_data isn't reused: its manual
    // mip generation re-uploads the same full-res buffer at every declared LOD size without
    // actually downsampling it — a pre-existing bug in that class, not touched here, just not
    // relied upon). Regenerated only when the ambient/tint sliders that feed the generator
    // change (see render_realcolor_debug_panel()), not every recompute.
    struct RealColorEnvCache
    {
        unsigned int mirror_tex     = 0; // GL_RGBA8, REALCOLOR_ENV_MIRROR_W x REALCOLOR_ENV_MIRROR_H, sharp
        unsigned int irradiance_tex = 0; // GL_RGBA8, REALCOLOR_ENV_IRRADIANCE_W x _H, pre-averaged/blurred
        bool valid = false;
        bool gl_objects_created() const { return mirror_tex != 0; }
    };

    // NEOTKO_REALCOLOR_TAG s166 (item 4): single-buffered (no ping-pong — shells are opaque,
    // one z-tested rasterization pass already resolves the nearest surface, unlike RealColor's
    // translucent peel stack) G-buffer for the shells_lit.fs AO kernel. Native canvas
    // resolution, no REALCOLOR_SUPERSAMPLE — shells are large solid volumes, not thin toolpath
    // lines, so they don't share RealColor's sub-pixel aliasing problem. Created/destroyed by
    // ensure_shells_ao_fbo()/destroy_shells_ao_fbo(), only when the debug-gated path in
    // render_shells() is active.
    struct ShellsAOCache
    {
        int tex_w = 0, tex_h = 0;
        unsigned int fbo = 0;
        unsigned int gbuffer_tex = 0; // GL_RGBA32F: rgb = packed view-space normal, a = eye_z (mm), a<=0 = no geometry
        unsigned int depth_tex = 0;   // GL_DEPTH_COMPONENT24, z-test only, never sampled
        bool gl_objects_created() const { return fbo != 0; }
    };

    // NEOTKO_SHADOW_TAG s229 (Fase 2): real directional shadow map — the scene rendered depth-only
    // from the light's point of view, then sampled per-fragment in shells_lit.fs. Replaces nothing:
    // it ADDS object-on-object and self shadowing, which the old flattened-to-bed shells_shadow pass
    // structurally cannot do (its receiver plane is hardcoded at world z=0). The planar bed shadow
    // stays because the printbed is NOT one of these volumes and so receives no shadow from this map
    // — see render_volumes_shadow()'s note and the study's §6 follow-up.
    // Depth-only FBO: no color attachment at all (glDrawBuffer(GL_NONE)). Square, fixed resolution.
    struct ShadowMapCache
    {
        int res = 0;
        unsigned int fbo = 0;
        unsigned int depth_tex = 0; // GL_DEPTH_COMPONENT24, sampled manually (compare mode NONE)
        bool gl_objects_created() const { return fbo != 0 && depth_tex != 0; }
    };

    // NEOTKO_GCODE_REPROCESSOR schema v2 (s215): `by_tool`/`tool` mirror the engine's
    // RuleMode::ByTool + tool id (ExpertGCodeReprocessor.cpp) — false/-1 means "global", the old
    // Phase 1/3 behaviour. `tool` is only meaningful when by_tool is true (0-based extruder
    // index, shown as T<tool> in the panel — matches the real gcode T<n> command numbering, T0
    // is the first tool, unlike the 1-based layer display elsewhere in this panel).

    // NEOTKO_GCODE_REPROCESSOR — one editable "speed_multiplier" rule as shown in the panel.
    // Unrelated to the JSON's forward-compat "unknown type" rules, those aren't editable here and
    // are round-tripped as opaque blobs when re-saving (see GCodeViewer.cpp
    // render_expert_gcode_reprocessor_panel()).
    struct ExpertReprocessorRule
    {
        bool enabled = true;
        bool by_tool = false;
        int tool = 0;
        int layer_from = 0;
        int layer_to = -1; // -1 == "to end of file"
        int value = 100;   // M220 S<value>, percent
        // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i) — see ExpertGCodeReprocessor.cpp's
        // SpeedRule/build_wipetower_windows()/subtract_windows() for the engine side.
        bool avoid_wipetower = false;
    };

    // NEOTKO_GCODE_REPROCESSOR — one editable "fan_override" rule. Same shape as
    // ExpertReprocessorRule but `value` is a raw M106 S value (0-255), not a percent.
    struct ExpertReprocessorFanRule
    {
        bool enabled = true;
        bool by_tool = false;
        int tool = 0;
        int layer_from = 0;
        int layer_to = -1; // -1 == "to end of file"
        int value = 255;   // M106 S<value>, 0-255
        bool avoid_wipetower = false; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
    };

    // NEOTKO_GCODE_REPROCESSOR Phase 2 — one editable "z_offset" rule. `value` is an absolute
    // SET_GCODE_OFFSET Z value in mm (e.g. -0.03), clamped to [-0.3, 0.3] (s215, user request —
    // keeps a bad value in "underextrusion/aggressive ironing" territory, not physically
    // dangerous); the engine restores with a plain Z=0, not the inverse of `value` — see
    // OffsetRule in ExpertGCodeReprocessor.cpp for why.
    struct ExpertReprocessorOffsetRule
    {
        bool enabled = true;
        bool by_tool = false;
        int tool = 0;
        int layer_from = 0;
        int layer_to = -1; // -1 == "to end of file"
        double value = 0.0; // mm, absolute SET_GCODE_OFFSET Z value, clamped to [-0.3, 0.3]
        bool avoid_wipetower = false; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
    };

    // NEOTKO_GCODE_REPROCESSOR (s215) — one editable "flow_multiplier" rule. Same shape as
    // ExpertReprocessorRule (M220 speed) but M221 and clamped to [20, 200] instead of [1, 300].
    struct ExpertReprocessorFlowRule
    {
        bool enabled = true;
        bool by_tool = false;
        int tool = 0;
        int layer_from = 0;
        int layer_to = -1; // -1 == "to end of file"
        int value = 100;   // M221 S<value>, percent, clamped [20, 200]
        bool avoid_wipetower = false; // NEOTKO_GCODE_REPROCESSOR "Avoid Wipetower" (s216i)
    };

    // NEOTKO_GCODE_REPROCESSOR — panel state, live-edited, auto-saved to the
    // "expert_gcode_reprocessor_rules" project config option on every change (s215: dropped the
    // old "Apply" button — see render_expert_gcode_reprocessor_panel() for why).
    struct ExpertReprocessorPanelState
    {
        // NEOTKO_GCODE_REPROCESSOR (s215): master ON/OFF — mirrors the JSON root's "enabled" key.
        // When false, run_expert_gcode_reprocessor() no-ops entirely regardless of what rules
        // exist, without the user having to delete/disable every rule individually.
        bool enabled = true;
        std::vector<ExpertReprocessorRule> rules;
        std::vector<ExpertReprocessorFanRule> fan_rules;
        std::vector<ExpertReprocessorOffsetRule> offset_rules;
        std::vector<ExpertReprocessorFlowRule> flow_rules;
        // Opaque rule objects from the JSON whose "type" this build doesn't understand — kept
        // verbatim and re-emitted on save so an older build never destroys a newer build's rules.
        std::vector<nlohmann::json> unknown_rules;
        bool loaded_from_config = false;
        // NEOTKO_GCODE_REPROCESSOR (s216): GLOBAL/BY TOOL toggle for the chart in
        // render_expert_gcode_reprocessor_chart() — pure view filter, not persisted (deliberately
        // absent from the JSON save block), doesn't affect any rule's own by_tool flag.
        bool chart_by_tool_mode = true;
        // NEOTKO_GCODE_REPROCESSOR (s216b): transient chart-interaction state, never persisted.
        // A drag in progress is identified by (rule-type ordinal, index within that type's own
        // vector, which endpoint) — stable across frames because add/remove can't happen
        // mid-drag (both hands are on the mouse button the whole time).
        int chart_drag_kind  = -1; // RuleKind ordinal of the endpoint being dragged, -1 = none
        int chart_drag_index = -1;
        int chart_drag_end   = 0;  // 0 = layer_from endpoint, 1 = layer_to endpoint
        // Context captured when the right-click "add rule here" popup opens (popup outlives the
        // click by several frames, so the click position must be remembered, not re-read).
        int chart_add_tool  = -1;  // -1 = add a global rule (GLOBAL view)
        int chart_add_layer = 0;   // 0-based
        // NEOTKO_GCODE_REPROCESSOR (s216c): identity of the rule targeted by the right-click
        // "delete this rule" popup — same (kind ordinal, index) addressing as chart_drag_*.
        int chart_delete_kind  = -1;
        int chart_delete_index = -1;
    };

    //BBS
    ConflictResultOpt m_conflict_result;
private:
    std::vector<int> m_plater_extruder;
    bool m_gl_data_initialized{ false };
    unsigned int m_last_result_id{ 0 };
    size_t m_moves_count{ 0 };
    //BBS: save m_gcode_result as well
    const GCodeProcessorResult* m_gcode_result;
    //BBS: add only gcode mode
    bool m_only_gcode_in_preview {false};
    std::vector<size_t> m_ssid_to_moveid_map;

    std::vector<TBuffer> m_buffers{ static_cast<size_t>(EMoveType::Extrude) };
    // bounding box of toolpaths
    BoundingBoxf3 m_paths_bounding_box;
    // bounding box of toolpaths + marker tools
    BoundingBoxf3 m_max_bounding_box;
    //BBS: add shell bounding box
    BoundingBoxf3 m_shell_bounding_box;
    float m_max_print_height{ 0.0f };

    //BBS save m_tools_color and m_tools_visible
    ETools m_tools;
    ConfigOptionMode m_user_mode;
    bool m_fold = {false};

    Layers m_layers;
    std::array<unsigned int, 2> m_layers_z_range;
    std::vector<ExtrusionRole> m_roles;
    size_t m_extruders_count;
    std::vector<unsigned char> m_extruder_ids;
    std::vector<float> m_filament_diameters;
    std::vector<float> m_filament_densities;
    Extrusions m_extrusions;
    SequentialView m_sequential_view;
    IMSlider* m_moves_slider;
    IMSlider* m_layers_slider;
    Shells m_shells;
    // NEOTKO_REALCOLOR_TAG: TD/rgb LUT for RealColor view, see refresh_realcolor_materials()
    RealColorMaterials m_realcolor_materials;
    // NEOTKO_REALCOLOR_TAG: depth-peel/accumulate GL scratch + idle-cache, see render_toolpaths_realcolor()
    RealColorCache m_realcolor_cache;
    // NEOTKO_REALCOLOR_TAG: debug-tunable lighting/background constants, see RealColorTuning above
    RealColorTuning m_realcolor_tuning;
    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): procedural equirect environment textures, see
    // RealColorEnvCache above / ensure_realcolor_env_textures()
    RealColorEnvCache m_realcolor_env;
    // NEOTKO_REALCOLOR_TAG s166 (item 4): G-buffer for the shells Phong+SSAO path, see ShellsAOCache above
    ShellsAOCache m_shells_ao_cache;
    // NEOTKO_SHADOW_TAG s229 (Fase 2): directional shadow map, see ShadowMapCache above.
    ShadowMapCache m_shadow_map_cache;
    // World -> light clip transform for the current frame's shadow map, and the world-space size of
    // one of its texels (mm) which drives the normal-offset bias. Both recomputed by
    // render_shadow_map() and consumed by render_volumes_lit() when setting shells_lit's uniforms.
    // Deliberately a raw Matrix4d, NOT a Transform3d: it is a projection*view product, and
    // Transform3d's affine fast path is exactly what corrupted the Align & Stack projection in s227
    // (see render_shadow_map()'s own note). An orthographic projection happens to be affine, so
    // Transform3d would work today — the raw type makes it impossible to break by swapping in a
    // perspective/spot light later.
    Matrix4d m_shadow_light_proj_view{ Matrix4d::Identity() };
    float m_shadow_texel_world_mm{ 0.0f };
    // NEOTKO_GCODE_REPROCESSOR — LibreMode-gated rule editor panel state, see
    // render_expert_gcode_reprocessor_panel().
    ExpertReprocessorPanelState m_expert_reprocessor;
    /*BBS GUI refactor, store displayed items in color scheme combobox */
    std::vector<EViewType> view_type_items;
    std::vector<std::string> view_type_items_str;
    int       m_view_type_sel = 0;
    EViewType m_view_type{ EViewType::FeatureType };
    std::vector<EMoveType> options_items;

    bool m_legend_enabled{ true };
    float m_legend_height;
    PrintEstimatedStatistics m_print_statistics;
    PrintEstimatedStatistics::ETimeMode m_time_estimate_mode{ PrintEstimatedStatistics::ETimeMode::Normal };
#if ENABLE_GCODE_VIEWER_STATISTICS
    Statistics m_statistics;
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    std::array<float, 2> m_detected_point_sizes = { 0.0f, 0.0f };
    GCodeProcessorResult::SettingsIds m_settings_ids;
    std::array<SequentialRangeCap, 2> m_sequential_range_caps;

    std::vector<CustomGCode::Item> m_custom_gcode_per_print_z;

    bool m_contained_in_bed{ true };
mutable bool m_no_render_path { false };
    bool m_is_dark = false;

public:
    GCodeViewer();
    ~GCodeViewer();

    void on_change_color_mode(bool is_dark);
    float m_scale = 1.0;
    void set_scale(float scale = 1.0);
    void init(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle);
    void update_by_mode(ConfigOptionMode mode);

    // extract rendering data from the given parameters
    //BBS: add only gcode mode
    void load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume,
            const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode = false);
    // recalculate ranges in dependence of what is visible and sets tool/print colors
    void refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors);
    void refresh_render_paths();
    void update_shells_color_by_extruder(const DynamicPrintConfig* config);
    void set_shell_transparency(float alpha = 0.15f);

    void reset();
    //BBS: always load shell at preview
    void reset_shell();
    void load_shells(const Print& print, bool initialized, bool force_previewing = false);
    void set_shells_on_preview(bool is_previewing) { m_shells.previewing = is_previewing; }
    //BBS: add all plates filament statistics
    void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show = true) const;
    //BBS: GUI refactor: add canvas width and height
    void render(int canvas_width, int canvas_height, int right_margin);
    //BBS
    void _render_calibration_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    void _render_calibration_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);

    bool has_data() const { return !m_roles.empty(); }
    bool can_export_toolpaths() const;
    std::vector<int> get_plater_extruder();

    const float                get_max_print_height() const { return m_max_print_height; }
    const BoundingBoxf3& get_paths_bounding_box() const { return m_paths_bounding_box; }
    const BoundingBoxf3& get_max_bounding_box() const { return m_max_bounding_box; }
    const BoundingBoxf3& get_shell_bounding_box() const { return m_shell_bounding_box; }
    const std::vector<double>& get_layers_zs() const { return m_layers.get_zs(); }
    const std::array<unsigned int,2> &get_layers_z_range() const { return m_layers_z_range; }

    const SequentialView& get_sequential_view() const { return m_sequential_view; }
    void update_sequential_view_current(unsigned int first, unsigned int last);

    /* BBS IMSlider */
    IMSlider *get_moves_slider() { return m_moves_slider; }
    IMSlider *get_layers_slider() { return m_layers_slider; }
    void enable_moves_slider(bool enable) const;
    void update_moves_slider(bool set_to_max = false);
    void update_layers_slider_mode();
    void update_marker_curr_move();

    bool is_contained_in_bed() const { return m_contained_in_bed; }
    //BBS: add only gcode mode
    bool is_only_gcode_in_preview() const { return m_only_gcode_in_preview; }

    EViewType get_view_type() const { return m_view_type; }
    void set_view_type(EViewType type, bool reset_feature_type_visible = true) {
        if (type == EViewType::Count)
            type = EViewType::FeatureType;
        m_view_type = (EViewType)type;
        if (reset_feature_type_visible && type == EViewType::ColorPrint) {
            reset_visible(EViewType::FeatureType);
        }
    }
    void reset_visible(EViewType type) {
        if (type == EViewType::FeatureType) {
            for (size_t i = 0; i < m_roles.size(); ++i) {
                ExtrusionRole role = m_roles[i];
                m_extrusions.role_visibility_flags = m_extrusions.role_visibility_flags | (1 << role);
            }
        } else if (type == EViewType::ColorPrint){
            m_tools.m_tool_visibles.assign(m_tools.m_tool_visibles.size(), true);
        }
    }

    bool is_toolpath_move_type_visible(EMoveType type) const;
    void set_toolpath_move_type_visible(EMoveType type, bool visible);
    unsigned int get_toolpath_role_visibility_flags() const { return m_extrusions.role_visibility_flags; }
    void set_toolpath_role_visibility_flags(unsigned int flags) { m_extrusions.role_visibility_flags = flags; }
    unsigned int get_options_visibility_flags() const;
    void set_options_visibility_from_flags(unsigned int flags);
    void set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range);

    bool is_legend_enabled() const { return m_legend_enabled; }
    void enable_legend(bool enable) { m_legend_enabled = enable; }
    float get_legend_height() { return m_legend_height; }

    void export_toolpaths_to_obj(const char* filename) const;

    std::vector<CustomGCode::Item>& get_custom_gcode_per_print_z() { return m_custom_gcode_per_print_z; }
    size_t get_extruders_count() { return m_extruders_count; }
    void push_combo_style();
    void pop_combo_style();

    // NEOTKO_REALCOLOR_TAG s166 (item 4), promoted to LibreMode s228: generic version of
    // render_shells_lit()'s Phong+fresnel+SSAO(+shadow) pipeline, parametrized over an arbitrary
    // GLVolumeCollection/ERenderType/filter instead of hardcoding m_shells.volumes+Transparent, so
    // GLCanvas3D::_render_objects() can reuse it against the Prepare tab's own m_volumes. Shares
    // the same shells_gbuffer/shells_lit/shells_shadow shaders and m_shells_ao_cache FBO as the
    // Preview-only path (see render_shells_lit below, now a thin wrapper over this). Returns false
    // (caller falls back to its normal shader) on any missing shader/FBO — see
    // ensure_shells_ao_fbo's own fallback note.
    bool render_volumes_lit(GLVolumeCollection& volumes, GLVolumeCollection::ERenderType type, bool depth_mask, bool with_shadow,
                             int canvas_width, int canvas_height, const Camera& camera,
                             const std::function<bool(const GLVolume&)>& filter = std::function<bool(const GLVolume&)>(),
                             bool partly_inside_enable = true);
    // NEOTKO_LIBREMODE_TAG s228: standalone contact-shadow pass split out of render_volumes_lit —
    // see its definition (GCodeViewer.cpp) for why: callers whose bed is drawn AFTER their object
    // pass (GLCanvas3D's Prepare tab) must call this separately, later in the frame, once the bed
    // is actually in the depth/color buffer for the shadow to test against and blend onto.
    void render_volumes_shadow(GLVolumeCollection& volumes, GLVolumeCollection::ERenderType type,
                                int canvas_width, int canvas_height, const Camera& camera,
                                const std::function<bool(const GLVolume&)>& filter = std::function<bool(const GLVolume&)>(),
                                bool partly_inside_enable = true);

private:
    void load_toolpaths(const GCodeProcessorResult& gcode_result, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box);
    //BBS: always load shell at preview
    //void load_shells(const Print& print);
    void refresh_render_paths(bool keep_sequential_current_first, bool keep_sequential_current_last) const;
    // NEOTKO_REALCOLOR_TAG: rebuilds m_realcolor_materials from raw filament colors + app_config TD
    void refresh_realcolor_materials(const std::vector<std::string>& str_tool_colors);
    // NEOTKO_REALCOLOR_TAG: depth-peel + Beer-Lambert accumulate pipeline, idle-cached; falls
    // back to the cheap Tool-equivalent render while the camera is moving. See plan doc.
    void render_toolpaths_realcolor(int canvas_width, int canvas_height);
    bool ensure_realcolor_fbos(int w, int h);
    void destroy_realcolor_fbos();
    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3, docs/WIP/REALCOLOR_VIEW/09_HDR_ENVIRONMENT_PLAN.md):
    // lazily creates + (re)generates the procedural equirect env textures. force_regen=true
    // re-bakes the pixel content even if the GL objects already exist (tuning changed).
    bool ensure_realcolor_env_textures(bool force_regen);
    void destroy_realcolor_env_textures();
    // NEOTKO_REALCOLOR_TAG: debug-only ImGui panel exposing RealColorTuning as live sliders,
    // gated by NeoDebug::enabled(NeoDebug::REALCOLOR) — no-op unless ORCA_DEBUG_REALCOLOR is set
    void render_realcolor_debug_panel();
    // NEOTKO_GCODE_REPROCESSOR — PRO-only rule editor, gated on app_config "neotko_libre_mode"
    // (not a NeoDebug channel — this is a real user-facing feature). s216: renders inline inside
    // the already-open "Legend" ImGui window, called only when m_view_type ==
    // EViewType::GCodeReprocessor (see render_legend()'s early-return branch, right after the
    // m_fold check) — no longer its own floating window. See ExpertGCodeReprocessor.cpp for the
    // engine that consumes the JSON this panel writes to config option
    // "expert_gcode_reprocessor_rules".
    void render_expert_gcode_reprocessor_panel();
    // NEOTKO_GCODE_REPROCESSOR (s216 Fase 2, interactive since s216b, sole editor since s216c) —
    // drawn near the top of render_expert_gcode_reprocessor_panel(): GLOBAL/BY TOOL toggle + a
    // per-tool bar chart (one lane per rule type, colored) + a plain-text summary, all over
    // m_expert_reprocessor. This is the ONLY editor now (the old per-type checkbox/DragInt rows
    // were removed once this covered full CRUD): dragging an endpoint dot edits that rule's
    // layer_from/layer_to (top of the chart snaps to "END"); clicking a summary row's value badge
    // opens a popup to type its number (percent/S-value/mm); right-clicking empty chart space
    // opens an "add rule here" popup; right-clicking an existing point opens a "delete this rule"
    // popup. All the drag/popup state lives in ExpertReprocessorPanelState's chart_* fields above.
    // Returns true when an edit committed (drag released, value typed, rule added/deleted) so the
    // caller folds it into the panel's auto-save accumulator. Tool columns are
    // T0..T(extruders_count-1), 0-based to match the real gcode T<n> command — NOT 1-based like
    // the layer fields in this panel.
    bool render_expert_gcode_reprocessor_chart();
    RealColorFingerprint compute_realcolor_fingerprint(int canvas_width, int canvas_height) const;
    void render_toolpaths();
    void render_shells(int canvas_width, int canvas_height);
    // NEOTKO_REALCOLOR_TAG s166 (item 4): G-buffer for the shells Phong+SSAO path — see
    // ShellsAOCache. Shared by render_shells_lit() (Preview) and render_volumes_lit() (Prepare,
    // see public section above), both gated on app_config "neotko_libre_mode".
    bool ensure_shells_ao_fbo(int w, int h);
    void destroy_shells_ao_fbo();
    // NEOTKO_SHADOW_TAG s229 (Fase 2): directional shadow map — see ShadowMapCache. Depth-only FBO,
    // square, fixed resolution (SHADOW_MAP_RES in GCodeViewer.cpp).
    bool ensure_shadow_map_fbo(int res);
    void destroy_shadow_map_fbo();
    // Fits an orthographic light frustum to `volumes`' world bounding box along the world-space key
    // light direction, writes m_shadow_light_proj_view + m_shadow_texel_world_mm, and renders the
    // depth-only pass into m_shadow_map_cache. Returns false if the shader/FBO is unavailable or the
    // collection has no usable bounding box, in which case render_volumes_lit() proceeds with
    // u_shadow_enabled=false (AO + SSCS still apply — nothing looks broken, just no cast shadow).
    // (No ERenderType parameter on purpose: the depth pass always uses ERenderType::All, because a
    // caster must never be missing from the map just for being transparent in the camera pass.)
    bool render_shadow_map(GLVolumeCollection& volumes,
                           const std::function<bool(const GLVolume&)>& filter, bool partly_inside_enable);
    // NEOTKO_REALCOLOR_TAG s166 (item 4): thin wrapper over render_volumes_lit() bound to
    // m_shells.volumes+Transparent, kept for render_shells()'s existing call site. Returns false
    // (caller falls back to plain gouraud_light) if any shader/FBO isn't available.
    bool render_shells_lit(int canvas_width, int canvas_height, const Camera& camera);

    //BBS: GUI refactor: add canvas size
    void render_legend(float &legend_height, int canvas_width, int canvas_height, int right_margin);
    void render_slider(int canvas_width, int canvas_height);

#if ENABLE_GCODE_VIEWER_STATISTICS
    void render_statistics();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
    bool is_visible(ExtrusionRole role) const {
        return role < erCount && (m_extrusions.role_visibility_flags & (1 << role)) != 0;
    }
    bool is_visible(const Path& path) const { return is_visible(path.role); }
    void log_memory_used(const std::string& label, int64_t additional = 0) const;
    ColorRGBA option_color(EMoveType move_type) const;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeViewer_hpp_

