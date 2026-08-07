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

        // NEOTKO_REALCOLOR_TAG s251 (Fase 0 del brillo anisótropo, docs/FUTURE/
        // REALCOLOR_ANISOTROPIC_SHEEN_PLAN.md): acumulador del EJE de impresión de este Path.
        //
        // EL PORQUÉ, en una línea: una superficie FDM es un haz de cilindros paralelos y refleja
        // como el pelo o el metal cepillado — la franja de brillo es perpendicular al cordón. Ese
        // dato está en el toolpath y hoy se tira. En los FLANCOS se recupera gratis desde la normal
        // (realcolor_peel.vs lo hace), pero en las caras de ARRIBA la normal es ±Z pura y no sabe
        // nada de la dirección: ahí hace falta este dato. Y es justo la cara que importa para un
        // top surface y para un sandwich.
        //
        // 🔑 SE ACUMULA EL EJE, NO LA DIRECCIÓN. Un cordón no tiene sentido: mira igual hacia
        // delante que hacia atrás. Un top monotónico es un zigzag →,←,→,← cuyas DIRECCIONES se
        // cancelan (media ≈ 0) mientras que sus EJES son todos el mismo. La forma estándar de
        // promediar ejes es duplicar el ángulo: se acumula (cos 2θ, sin 2θ) en vez de (cos θ,
        // sin θ), de modo que 0° y 180° caen en el mismo sitio. Es la MISMA simetría de orden 2
        // que hace que el TD del cordón se ajuste como A + B·cos(2θ) — aquí usada para promediar
        // en vez de para ajustar. Sin trigonometría: con d = (dx, dy) normalizado en XY,
        //     cos 2θ = dx² − dy²   y   sin 2θ = 2·dx·dy.
        //
        // 🎁 Y sale gratis un indicador de CONFIANZA: el módulo del vector medio. Cerca de 1 =
        // todas las líneas comparten eje (un top monotónico); cerca de 0 = no hay eje dominante
        // (un perímetro externo, que es una vuelta completa y por tanto un Path que apunta a todas
        // partes). O sea que el efecto se apaga SOLO donde no tiene sentido, sin una lista de roles
        // escrita a mano — que es justo lo que pedía la lección del erExternalPerimeter (s243):
        // un parámetro que sólo se aparta del neutro donde nadie mira es un parámetro que no existe.
        //
        // Formato: (Σ w·cos2θ, Σ w·sin2θ, Σ w), con w = longitud XY del segmento. Se rellena
        // durante la generación de vértices (add_vertices_as_solid), que es donde `dir` ya está
        // calculada — coste cero, no se recorre nada dos veces. Se resuelve a un eje unitario con
        // realcolor_bead_axis() en GCodeViewer.cpp. Ponderar por longitud es lo correcto: los
        // micro-segmentos de una esquina redondeada no deben pesar como una scanline entera.
        //
        // ⚠️ Ponderar por longitud XY y no 3D a propósito: un movimiento puramente vertical (Z
        // hop extruido, raro pero existe) no tiene eje en el plano y debe aportar 0, no ruido.
        Vec3f bead_axis_acc{ Vec3f::Zero() };

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
        // NEOTKO: true SOLO cuando el usuario ha movido el tirador de inicio fuera de su posicion por
        // defecto. Con el tirador al minimo esto es false y el rango inferior NO recorta nada — asi las
        // capas de debajo se siguen dibujando (en gris) como en el modo de capas apiladas de siempre.
        bool first_limited{ false };
        // NEOTKO: true cuando el tirador que se manipula es el de INICIO del rango. Marca a que extremo
        // siguen el toolhead virtual y la ventana de gcode textual.
        bool track_first{ false };
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
    // NEOTKO_REALCOLOR_TAG s251b: ACABADO POR FILAMENTO. Los presets vienen de PHOTO_MODE_PLAN.md
    // §8 (F3), la tabla que ya está shippeada en el renderer de shells — se reusan sus nombres y su
    // orden relativo en vez de inventar una escala nueva. `Custom` no es un preset elegible: es lo
    // que se muestra cuando el usuario mueve un slider a mano y los valores dejan de coincidir con
    // ninguna fila de la tabla.
    enum class RealColorFinishPreset : int { Plastic = 0, Glossy, Matte, Rubber, Metal, Silk, Custom };

    struct RealColorMaterials
    {
        std::array<ColorRGB, 4> rgb{};
        std::array<float, 4>    td{};

        // NEOTKO_REALCOLOR_TAG s251b: acabado óptico por SLOT (0-3), como el td de arriba y por el
        // mismo motivo — es una propiedad del material, no de la geometría. Cada entrada es
        // (gloss_mul, rough_offset, aniso_mul), y los tres son MODIFICADORES sobre la tabla por
        // role de realcolor_surface_finish(), no sustitutos de ella:
        //
        //   gloss_mul    — multiplica. Neutro 1.0.
        //   rough_offset — SUMA a la rugosidad del role. Neutro 0.0.
        //   aniso_mul    — multiplica la fuerza del brillo anisótropo. Neutro 1.0.
        //
        // 🔑 POR QUÉ LA RUGOSIDAD SUMA Y LAS OTRAS DOS MULTIPLICAN, que no es un capricho: la
        // rugosidad es una magnitud ACOTADA en 0..1, y multiplicarla aplastaría las diferencias
        // entre roles justo en los materiales mate (un top a 0.15 y un bridge a 0.95, por 1.5,
        // saturan los dos contra el techo y se vuelven indistinguibles). Sumando un desplazamiento,
        // la distancia entre roles se conserva intacta y el material mueve la superficie ENTERA
        // hacia lo rugoso o hacia lo liso — que es lo que hace un material de verdad. El brillo y
        // la anisotropía no están acotados por arriba, así que ahí multiplicar es lo natural.
        //
        // ⚠️ Neutro global = (1.0, 0.0, 1.0) en los cuatro slots ≡ comportamiento pre-s251b exacto.
        //
        // 🚨 EL NEUTRO SE ESCRIBE AQUÍ EXPLÍCITO, no se deja en `{}`. Un `{}` value-inicializa a
        // CEROS, y cero en gloss_mul no es "sin efecto": es brillo cero, o sea la escena apagada.
        // Cualquier camino que dibujase antes de que refresh_realcolor_materials() rellenase esto
        // (o con más de 4 extrusores, o un app_config a medio leer) daría una imagen negra en vez
        // de la de siempre. El neutro de un modificador multiplicativo es 1, no 0.
        std::array<std::array<float, 3>, 4> finish{ { { 1.0f, 0.0f, 1.0f },
                                                      { 1.0f, 0.0f, 1.0f },
                                                      { 1.0f, 0.0f, 1.0f },
                                                      { 1.0f, 0.0f, 1.0f } } };
        // Preset elegido por slot, sólo para que la UI sepa qué mostrar en el combo. El valor que
        // manda SIEMPRE es `finish`; esto es una etiqueta, no una fuente de verdad.
        std::array<int, 4> finish_preset{};

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
        // NEOTKO_REALCOLOR_TAG s251b: el acabado por filamento ENTRA en el fingerprint. Sin esto la
        // ventana de materiales parecería no hacer nada: el composite de peel está cacheado y sólo
        // se recalcula cuando este struct cambia, así que cambiar un material a Mate habría dejado
        // en pantalla la imagen vieja hasta que el usuario moviese la cámara. Es el mismo motivo
        // por el que ya estaban aquí td y rgb.
        std::array<std::array<float, 3>, 4> finish{};
        unsigned int layers_z_range[2] = { 0, 0 };
        unsigned int moves_first = 0, moves_last = 0;

        bool operator==(const RealColorFingerprint& other) const {
            return canvas_w == other.canvas_w && canvas_h == other.canvas_h &&
                   layers_z_range[0] == other.layers_z_range[0] && layers_z_range[1] == other.layers_z_range[1] &&
                   moves_first == other.moves_first && moves_last == other.moves_last &&
                   td == other.td && rgb == other.rgb && finish == other.finish &&
                   view == other.view && proj == other.proj;
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
        // NEOTKO_REALCOLOR_TAG s249: TODOS los defaults de este struct son ahora la calibración que
        // el usuario cerró en s249, con las tres perillas nuevas de swell ya dentro de la ecuación.
        // Los valores de s243/s243d/s166 que se citan en los comentarios de abajo se conservan como
        // historia de POR QUÉ cada uno se movió — no son los vigentes.
        float accum_seed_gray  = 0.013f; // linear-space accumulator background, see item 1

        // NEOTKO_REALCOLOR_TAG s251c: LUZ DIRECTA, ahora con mandos. El diagnóstico completo (por
        // qué la veta de los contornos no se podía apagar, y por qué era la DIFUSA y no el
        // especular) está en 140/realcolor_peel.vs. Neutro ≡ pre-s251c = (0.48, 0.18, 0.075, 20, 0).
        //
        // 🔑 EL REPARTO DE ENERGÍA, QUE ES DE DONDE SALEN ESTOS NÚMEROS Y NO DE MI GUSTO.
        // Sobre la sección de un cordón (un cilindro), la media de max(N·L, 0) vale 1/π ≈ 0.318 del
        // pico. O sea que con Lambert puro la generatriz iluminada es **3.14 veces** más brillante
        // que la media del propio cordón: ÉSE es el número que dibuja la veta, y no depende de la
        // intensidad — sólo del modelo. Bajar la luz no lo cambia, la oscurece entera.
        //
        // Con difusa envolvente w, la media sube a (integrando (cosθ+w)/(1+w) sobre el arco no
        // recortado): w=0.5 → 0.406 · w=0.7 → 0.441 · w=1.0 → 0.500. El contraste pico/media cae a
        // 2.46 / 2.27 / 2.00 respectivamente. **Con w = 0.70 la veta pierde un 28% de contraste**,
        // que es el efecto que se busca, y es un cambio de MODELO, no de exposición.
        //
        // Fijado eso, la intensidad se elige para conservar el nivel medio de la imagen y de paso
        // pasar peso de la direccional al ambiente (que es lo que quita el look de "una sola luz
        // dura"): media direccional de hoy = 0.66 × 0.318 = 0.210. El objetivo es ~0.155, así que
        // key+fill = 0.155 / 0.441 = 0.351, repartido en la misma proporción 2.67:1 de siempre.
        // Lo que la direccional deja de aportar lo recogen ambient_ground/sky, subidos abajo.
        float light_key       = 0.384f;  // era 0.48 (0.8 × 0.6)
        float light_fill      = 0.197f;  // era 0.18 (0.3 × 0.6)
        // El especular no era el culpable (verificado por el usuario poniendo el Gloss del material
        // a 0), pero un lóbulo BLANCO y ESTRECHO sobre un base saturado sigue siendo la señal
        // visual de "metal". Se baja la amplitud y se ENSANCHA (20 → 6): la línea fina se convierte
        // en un lustre ancho, que es lo que hace un plástico.
        float light_spec      = 0.045f; // era 0.075 (0.125 × 0.6)
        float light_shininess = 6.0f;   // era 20.0
        float light_wrap      = 0.70f;  // era 0.0 (Lambert puro)

        // NEOTKO_REALCOLOR_TAG s251d: AISLADOR DE TÉRMINOS, herramienta de diagnóstico. 0 = imagen
        // normal. Los 7 modos y qué prueba cada uno están documentados en 140/realcolor_peel.fs
        // (bloque de u_light_solo). No es un ajuste: no tiene sentido dejarlo distinto de 0.
        int light_solo = 0;

        // NEOTKO_REALCOLOR_TAG s251c: SUBIDOS (0.337→0.42, 0.420→0.52) como contrapartida de bajar
        // la direccional. No es un retoque estético suelto: es la otra mitad del mismo reparto de
        // arriba, y separarlos dejaría la imagen oscura. Total medio ≈ el de antes, pero repartido
        // entre una luz que modela y un ambiente que rellena, en vez de casi todo en la direccional.
        float ambient_ground   = 0.302f; // realcolor_peel.vs two-tone ambient, see item 2
        float ambient_sky      = 0.466f;
        float fresnel_power    = 5.489f; // realcolor_peel.vs rim term, see item 2
        // s243d: 0.519, calibrado a ojo por el usuario contra su escena. El 0.05 anterior venía
        // de s164, cuando el rim era el ÚNICO sitio donde vivía cualquier variación de material;
        // con F4 repartiendo el acabado por la difusa, el rim puede pesar de verdad sin dominar.
        // s249: 0.479 — retoque fino al recalibrar la escena entera con el swell proporcional.
        float fresnel_strength = 0.205f;
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
        // s249: subidos bastante al recalibrar (0.35→0.660, 6→14.720). Con el swell proporcional
        // el cordón ya no se dibuja al doble de su ancho, así que hay menos plástico falso por el
        // que "difundir" — el SSS tiene que trabajar más para dar la misma sensación de material
        // translúcido. Los dos efectos se compensaban sin que se viera.
        float sss_strength     = 0.660f;
        float sss_radius_px    = 14.720f;
        float sss_reference_td = 0.941f;

        // NEOTKO_REALCOLOR_TAG s243 (F1, "huecos"): desplazamiento de cada vértice del toolpath a
        // lo largo de su normal, en mm de mundo — ver realcolor_peel.vs para el porqué físico (la
        // extrusión real solapa con su vecina, la geometría del visor no). Ambos a 0 =
        // comportamiento pre-s243.
        //
        // s243b — SEPARADOS por orientación de la cara, tras la prueba del usuario: a 0.1 uniforme
        // mejoraban unas zonas y a 0.014 otras distintas, porque un solo escalar estaba haciendo
        // dos trabajos opuestos. Sólo los FLANCOS cierran el hueco entre líneas contiguas; hinchar
        // las caras horizontales no cierra nada y además funde capas entre sí (pared abombada, sin
        // costura). Por eso el lateral puede ir alto y el vertical tiene que ir corto —
        // adicionalmente, el bias de peel de realcolor_peel.fs se acota con la altura REAL del
        // path, así que engordar mucho en Z le haría medir contra una altura que no existe.
        // s243d: valores elegidos por el usuario. Los dos estaban en el tope de su slider, así
        // que los topes se han subido (ver render_realcolor_debug_panel) — un default clavado en
        // el máximo no deja explorar hacia arriba, y aún quedan huecos que cerrar.
        // s243e: subidos al tope actual tras la prueba del usuario ("casi casi se ve sólido").
        // Estos SÍ se quedan clavados en el máximo del slider a propósito: es donde el resultado
        // convence, y ya se comprobó que subir más el vertical entra en conflicto con el bias del
        // peel. Si algún día hace falta más recorrido, el que puede crecer es el lateral.
        //
        // 🔑 s249 — Y SE DESCLAVARON DEL TOPE, que era justo el objetivo del plan. Al añadir la
        // parte proporcional (swell_lateral_k abajo) el usuario recalibró y bajó el absoluto de
        // 0.500 a 0.212, o sea a MENOS DE LA MITAD. El criterio de éxito que el plan escribió a
        // ciegas ("poder bajar a la mitad del efecto actual manteniendo los huecos cerrados") se
        // cumplió casi clavado. Lo que quedaba clavado en el tope no era un óptimo: era el
        // síntoma de que el mando estaba mal parametrizado.
        // 🔑 s251 — Y VOLVIÓ A BAJAR, POR SEGUNDA VEZ Y POR LA MISMA CAUSA. Al calibrar la escena
        // con el brillo anisótropo puesto, el usuario dejó el absoluto lateral en 0.114 (de 0.212,
        // que ya venía de 0.500 en s243) y subió la k de 0.630 a 1.101. O sea: el hinchado falso
        // pasó a ser CASI TODO proporcional, y su parte fija se ha reducido a menos de un cuarto
        // de la original en dos sesiones.
        //
        // Y esto ya es un patrón, no una coincidencia: cada vez que RealColor gana un mecanismo que
        // da sensación de material DE VERDAD, el usuario baja el que la falseaba. En s249 fue el
        // swell proporcional el que permitió bajar el absoluto y a cambio subió mucho el SSS; aquí
        // es el brillo anisótropo el que permite bajarlo otra vez. El swell engorda geometría para
        // tapar un hueco; el brillo no engorda nada y sin embargo hace leer la superficie como
        // plástico. Cuando el efecto honesto entra, el sucedáneo sobra — mismo razonamiento que
        // hizo que el gradeo de A7 acabara en 1.07 en vez de comprimiendo (ver grade_contrast).
        // s251c: recalibrado otra vez con la luz nueva (la envolvente cambia cuánto hueco se
        // "ve", así que el swell se re-mide con ella puesta). El lateral proporcional baja de
        // 1.101 a 0.860 y el vertical fijo sube de 0.028 a 0.048 — ver el aviso de abajo, que
        // este último es el que tiene consecuencias.
        // s251d: 0.200 — recalibrado con la luz ya controlable. Sube desde 0.114 porque con la
        // difusa envolvente y el ambiente en su sitio los huecos entre cordones vuelven a leerse.
        float swell_lateral_mm  = 0.205f; // flancos: los que cierran los huecos que se ven
        float swell_vertical_mm = 0.024f; // caras arriba/abajo: sólo para sellar la costura

        // NEOTKO_REALCOLOR_TAG s249 (vía 1 de docs/FUTURE/REALCOLOR_SWELL_WITHOUT_EXPANSION_PLAN.md):
        // parte PROPORCIONAL del hinchado, sumada a la absoluta de arriba —
        //     swell_flancos  = swell_lateral_mm  + swell_lateral_k  * ancho_del_path
        //     swell_vertical = swell_vertical_mm + swell_vertical_k * altura_del_path
        //
        // EL PORQUÉ. El absoluto son mm iguales para todos los cordones, y el hueco que hay que
        // cerrar NO es igual para todos: es proporcional al ancho. A 0.500 mm por lado sobre un
        // cordón de ~0.42 mm el dibujo sale a más del doble de su anchura real, y cuanto más fino
        // el cordón más desproporcionado — de ahí el efecto de "expansión" que el usuario ve al
        // comparar RealColor contra la vista Filament (s246). Un factor relativo cierra el hueco
        // igual de bien en toda la pieza sin engordar los finos.
        //
        // ⚠️ Ambos a 0 = comportamiento s243 EXACTO. Nacieron a 0 a propósito, conviviendo con el
        // absoluto, para poder A/B sin perder el look calibrado del usuario. **Los valores de
        // abajo son ya el reparto definitivo, fijado por el usuario en vivo en s249** — que era la
        // condición que el plan ponía para tocarlos ("con el usuario delante, NO en silencio").
        //
        // Cuánto vale esto en la práctica, sobre el reparto elegido (0.212 + 0.630·ancho):
        //   cordón de 0.42 mm → 0.477 mm  (era 0.500: el look típico se mantiene)
        //   cordón de 0.20 mm → 0.338 mm  (era 0.500: **-32%**, la desproporción de los finos)
        // Es exactamente el síntoma (a) del plan, corregido sin tocar el caso que ya gustaba.
        //
        // 🔑 El vertical proporcional es además MÁS SEGURO que el absoluto: la regla de s243 dice
        // que el hinchado en Z tiene que quedarse por debajo de ~1/4 de la capa más fina, o la
        // comparación de profundidad del peel (acotada con la altura REAL del path) mide contra
        // geometría que no existe — el fallo que arregló s166. Con un factor sobre la altura del
        // PROPIO path, un k ≤ 0.25 cumple esa regla por construcción y en TODAS las capas a la
        // vez, cosa que un valor en mm no puede hacer cuando la escena mezcla alturas (ALH).
        //
        // ⚠️ s249, ojo con el vertical: la garantía "≤1/4 de la altura" la cumple la parte
        // PROPORCIONAL (0.230 < 0.25), pero el absoluto de 0.015 mm se suma por encima y no
        // escala. En una capa de 0.08 mm el total sale 0.033 mm = 0.41 de la altura, todavía por
        // debajo del bias_cap del peel (media altura = 0.04) pero ya sin margen. Si algún día
        // aparecen capas descartadas en un perfil MUY fino, lo que hay que bajar es
        // swell_vertical_mm a 0, no la k.
        // ✅ s251f — EL AVISO DEL bias_cap ESTÁ RESUELTO, y conviene dejar escrito el cálculo porque
        // es el que hay que rehacer cada vez que se toque el vertical.
        //
        // El bias del peel se acota con bias_cap = altura_del_path / 2, y el hinchado vertical vale
        // swell_vertical_mm + swell_vertical_k · altura. El cruce está donde se igualan:
        //     0.024 + 0.130·h = 0.5·h   ⇒   h ≈ 0.065 mm
        //
        // Historia del número, que es la razón de vigilarlo: s251 lo tenía en h ≈ 0.089 mm y s251c
        // lo metió en h ≈ 0.152 mm — o sea DENTRO del rango normal de capa, con un perfil de 0.12 mm
        // cruzándolo. Al recalibrar con la luz ya controlable (s251f) el usuario bajó el fijo a la
        // mitad y el cruce se fue a 0.065 mm, por debajo de cualquier perfil real. Riesgo cerrado.
        //
        // Qué pasaría si volviera a cruzarse: el hinchado en Z se sale del cap, el peel trata la capa
        // que intenta detectar como "ya vista" y la descarta ENTERA del apilado de Beer-Lambert. Es
        // el fallo que arregló s166, con síntoma característico — "la capa de debajo del top
        // desaparece", **y depende del zoom** (bien de cerca, mal de lejos).
        //
        // La regla si vuelve: bajar swell_vertical_mm, la parte FIJA, que no escala con la capa y es
        // la única culpable del cruce. NUNCA la k — a 0.130 cumple "≤1/4 de la altura" por
        // construcción y en todas las capas a la vez, que es justo lo que un valor en mm no puede.
        //
        // ⚠️ Y ojo al reparto lateral, que ha dado un vuelco: la k baja de 0.860 a 0.145 mientras el
        // absoluto se queda en 0.205. O sea que el hinchado vuelve a ser casi todo FIJO, al revés de
        // la tendencia de s249-s251c. No lo racionalizo: se calibró a ojo con la luz nueva puesta y
        // es la primera vez que el usuario reporta cero huecos, que era el criterio.
        float swell_lateral_k  = 0.145f; // fracción del ANCHO del path
        float swell_vertical_k = 0.130f; // fracción de la ALTURA del path (tope 0.25, ver arriba)

        // NEOTKO_REALCOLOR_TAG s249 (vía 2 del mismo plan): multiplicador del hinchado LATERAL en
        // los roles que forman la silueta (erExternalPerimeter + erOverhangPerimeter). 1.0 =
        // neutro, o sea el comportamiento de siempre.
        //
        // EL PORQUÉ. El shader no sabe si un cordón tiene vecino al lado o aire. En los cordones
        // interiores el hinchado cierra un hueco real; en el borde expuesto no hay nada que cerrar
        // y lo único que hace es mover el contorno de la pieza hacia fuera — que es la parte de la
        // "expansión" que más se lee a simple vista. El perímetro externo ES la silueta, así que
        // bajar sólo ahí aproxima "hinchar únicamente hacia dentro" sin ningún dato nuevo.
        //
        // Se incluye erOverhangPerimeter a propósito: un overhang perimeter es el perímetro
        // externo de la pieza sobre un voladizo. Dejarlo fuera haría que la silueta se hinchase a
        // trozos, justo en las zonas donde más raro se vería.
        //
        // 🔑 s249: el usuario lo dejó en 0.42, o sea que la vía 2 SÍ hacía falta — la vía 1 sola
        // no bastó. Y no lo puso a 0: la silueta se sigue hinchando un poco (0.42 · el lateral
        // que toque), sólo que mucho menos que el interior. Tiene sentido físico: un perímetro
        // externo real también se aplasta y se funde con el perímetro de al lado por su cara
        // interior, no es un prisma flotando en el aire. Cero habría sido el otro extremo.
        float swell_external_scale = 0.50f;

        // NEOTKO_REALCOLOR_TAG s243 (F4 = A4+A5): interpolación global de los acabados por tipo de
        // superficie hacia el neutro (gloss=1, roughness=0, que ≡ pre-s243). 0 = apagado del todo
        // y bit-idéntico a antes, 1 = la tabla de realcolor_surface_finish() tal cual. Existe para
        // poder A/B el efecto entero de un tirón en vez de tener que revertir código.
        // s243d: 0.514 — el usuario prefiere el acabado a media fuerza. La tabla de
        // realcolor_surface_finish() se calibró para que 1.0 fuese ya perceptible (s243c), así
        // que la mitad es una elección estética, no una compensación de un efecto flojo.
        // s249: 0.400 al recalibrar. Baja un poco porque el swell proporcional ya diferencia los
        // roles por geometría (un perímetro externo y un infill ya no se dibujan al mismo grosor
        // aparente), así que el acabado por material no tiene que cargar solo con esa distinción.
        float finish_strength = 0.518f;

        // NEOTKO_REALCOLOR_TAG s243 (F5 = A7): gradeo de presentación en realcolor_present.fs.
        // Ambos a 1.0 = imagen idéntica a pre-s243.
        //
        // 🔑 s243e — LOS DEFAULTS SALIERON AL REVÉS DE LO QUE PREDECÍA EL PLAN, y merece la pena
        // dejarlo escrito. A7 (docs/WIP/REALCOLOR_PSEUDOREALISTIC.md) sostenía que el look "de
        // render de CAD" viene de color demasiado saturado y luz demasiado dura, y proponía
        // comprimir rango y desaturar ~10% — que es de donde salió el 0.92/0.92 inicial. Calibrando
        // contra la escena real, el usuario acabó en contraste 1.07 (ABRE el rango en vez de
        // comprimirlo) y saturación 1.00 (sin desaturar nada).
        //
        // La explicación de por qué el plan falló: A7 se escribió como sustituto barato del resto
        // de frentes, para tapar con gradeo lo que no se iba a arreglar de verdad. Pero F1 (huecos),
        // F3 (eje de la luz) y F4 (acabado por superficie) SÍ se hicieron, y quitaron el look
        // artificial en su origen. Aplanar encima ya sólo restaba vida a la imagen. Un
        // apaño-para-no-arreglar deja de tener sentido cuando arreglas la causa.
        float grade_contrast   = 1.08f;
        float grade_saturation = 0.87f; // s249: un pelo desaturado, el contraste 1.07 se mantiene

        // NEOTKO_REALCOLOR_TAG s243 (F6, "silueta opaca"): sella los huecos INTERIORES de la pieza
        // (los que quedan tras F1 y que son huecos reales, no aliasing) sin tocar el contorno, que
        // es donde la transparencia parcial ES el antialiasing de la silueta. Ver compute_interior()
        // en realcolor_present.fs. 0 = apagado y bit-idéntico a pre-F6.
        //
        // ⚠️ s243e: DEFAULT 0 — este efecto es INERTE en la práctica y hay que saberlo antes de
        // volver a tocarlo. Sólo sube el alfa de píxeles que YA tienen geometría (los de cobertura
        // 0 hacen discard antes de llegar), y los huecos reales que quedan tras F1 son justamente
        // de cobertura 0: agujeros de un píxel entero o más. Encima el radio de muestreo es de 1
        // píxel de salida, así que en un hueco de 2-3 píxeles los propios vecinos caen dentro del
        // hueco y tampoco cuenta como interior. Verificado por el usuario: 0 y 1 se ven igual.
        //
        // Se deja el código porque es correcto y barato de reactivar, pero a 0 no se cobran 8
        // muestras por píxel por algo que no se ve. Para que sirviera de verdad habría que
        // rellenar también los píxeles de cobertura 0, y eso obliga a inventar color Y profundidad
        // — con el riesgo de tapar agujeros REALES del modelo. Esa decisión no está tomada.
        //
        // 🆕 s251f — EL DEFAULT YA NO ES 0 (0.07), pero el análisis de arriba NO se ha vuelto a
        // verificar. El usuario lo dejó ahí en la tanda en la que por fin no le salen huecos. A 0.07
        // el efecto es mínimo por construcción, así que lo honesto es escribir que **no está
        // comprobado que este valor concreto haga nada**: pudo quedarse de rebote al mover otros
        // mandos. Si alguien retoma F6, el punto de partida es el párrafo de arriba (sólo sube el
        // alfa de píxeles que YA tienen geometría), no este default.
        float fill_interior = 0.07f;

        // NEOTKO_REALCOLOR_TAG s251 (Fase 0 de docs/FUTURE/REALCOLOR_ANISOTROPIC_SHEEN_PLAN.md):
        // BRILLO ANISÓTROPO del cordón. Neutro = aniso_strength 0 → imagen bit-idéntica a s249b.
        //
        // QUÉ ES. Una superficie FDM no refleja como una superficie lisa: es un haz de cilindros
        // paralelos, y refleja como el pelo, la seda o el metal cepillado. El reflejo no es una
        // mancha redonda, es una FRANJA perpendicular al cordón. Por eso una pieza cambia tanto de
        // aspecto al girarla bajo una lámpara, y por eso un top planchado a 45° no brilla igual que
        // el mismo top a 0°. El modelo es Kajiya-Kay (el estándar de pelo), que sólo necesita el eje
        // del cordón (T) y ya lo tenemos: en los flancos derivado de la normal, en las caras
        // horizontales desde Path::bead_axis_acc. Ver realcolor_peel.{vs,fs}.
        //
        // 🚨 POR QUÉ ES UN TÉRMINO ADITIVO NUEVO Y NO UNA MODIFICACIÓN DEL ESPECULAR EXISTENTE.
        // §2.2 del plan avisaba de que la difusa de RealColor se calcula POR VÉRTICE
        // (realcolor_peel.vs, intensity.x), o sea que cualquier efecto que dependa de la normal por
        // fragmento sólo toca términos residuales — y la lección de s243c dice que un parámetro que
        // sólo toca residuales no existe. La salida es NO tocar la difusa: este lóbulo trae su
        // propia energía y su propio mando, así que puede hacerse ver sin bajar el sombreado al
        // fragment (que era la fase cara y arriesgada, con los dos gemelos de shader por medio).
        // Si la Fase 0 convence, ESA obra ya estará justificada; si no convence, nos la ahorramos.
        //
        // aniso_strength  — 0 = apagado (neutro). Multiplica el lóbulo entero.
        // aniso_sharpness — exponente del lóbulo. Alto = franja fina y metálica, bajo = brillo
        //                   ancho y satinado. NO es rugosidad: no toca el muestreo del entorno.
        // aniso_min_conf  — confianza mínima del eje para que el efecto se aplique. Por debajo, el
        //                   Path no tiene eje dominante (típicamente un perímetro externo, que es
        //                   un bucle cerrado) y se apaga con un fundido, no con un salto.
        //                   ⚠️ NO afecta a los flancos: ahí el eje es exacto por vértice, no promediado.
        // 🏁 s251 — CALIBRADO POR EL USUARIO Y ACEPTADO ("lo veo muy bien"). Nacieron con fuerza 0
        // (neutro) para poder A/B sin recompilar; estos son ya los valores definitivos. Tres cosas
        // que estos números dicen y que conviene no perder:
        //
        // 1. **La Fase 0 pasó su propio criterio de éxito.** El plan decía que si el efecto no se
        //    apreciaba, la Fase 2 (bajar difusa+especular al fragment, los dos gemelos de shader) NO
        //    se hacía. Se aprecia, y encima sin tocar la difusa — o sea que la apuesta de meter el
        //    lóbulo como término ADITIVO propio en vez de modular los residuales fue la correcta.
        //    Ver docs/VIDEOS/images/"RealColor - Quality.png" (gcode contra RealColor, el ángulo de
        //    extrusión cambiando el resultado) y RealColor-angleview.gif (la franja de brillo
        //    barriendo con la cámara, que es LA firma de un reflejo anisótropo: si fuera isótropo,
        //    el brillo seguiría a la cámara como una mancha en vez de barrer a lo ancho del cordón).
        //
        // 2. **aniso_min_conf = 0.84 es alto, y eso valida la métrica de confianza.** Sólo se
        //    aplica el brillo donde el eje del path está MUY definido. Si la confianza fuese ruido,
        //    un umbral tan alto habría apagado el efecto entero y el usuario habría acabado bajándolo
        //    a 0. Que sirva de mando útil significa que separa de verdad "top monotónico" de "vuelta
        //    de perímetro". ⚠️ Ojo: esto sólo filtra las TAPAS — los flancos llegan al shader con
        //    confianza 1 por vértice (su eje es exacto, no promediado) y nunca ven este umbral.
        //
        // 3. ⚠️ **aniso_sharpness = 170 sobre un tope de slider de 200: queda poco recorrido hacia
        //    arriba.** No está clavado en el máximo (la lección de s249 era sobre defaults PEGADOS al
        //    tope), pero está cerca. Si en alguna escena hiciera falta una franja aún más fina, lo
        //    que hay que subir es el tope del slider en render_realcolor_debug_panel(), no buscarle
        //    la vuelta por otro parámetro.
        float aniso_strength  = 0.196f;
        float aniso_sharpness = 170.0f;
        float aniso_min_conf  = 0.84f;

        // NEOTKO_REALCOLOR_TAG s251: visor de diagnóstico del eje, NO un efecto. 0 = apagado.
        // 1 = pinta el color por el eje recuperado (matiz = ángulo del eje, saturación =
        // confianza), de modo que un top monotónico debe salir de un color plano y uniforme, y un
        // perímetro externo debe salir GRIS (confianza 0). Existe porque el plan pedía MEDIR antes
        // de decidir, y mirar la imagen es más barato y más honesto que leerse un log de miles de
        // líneas: si esto no sale plano sobre un top, el dato está mal y el brillo sobraría.
        float aniso_debug = 0.0f;
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
    // NEOTKO_PHOTOMODE_TAG s242: did render_shadow_map() succeed for the CURRENT frame? Needed
    // because the Photo Mode cyclorama is drawn later in the frame, from GLCanvas3D, and has to
    // know whether the map it is about to sample was actually filled. A stale `true` would make
    // it sample last frame's depth texture (or an uninitialised one) and stripe the floor.
    bool m_shadow_map_valid_this_frame{ false };

    // NEOTKO_PHOTOMODE_TAG s242 (F5): the Photo Mode environment probe — two equirectangular
    // textures baked on the CPU from the Photo Mode lights, sampled by shells_lit.fs for the
    // ambient (irradiance) and the reflection (mirror).
    //
    // Deliberately NOT RealColor's m_realcolor_env, even though the baking technique is the same
    // (s214): that one is baked from RealColorTuning and lives in a Y-up space, and it is edited
    // from a different panel. Sharing it would mean the Preview's debug sliders silently changing
    // how a customer photo looks. Same idea, separate probe.
    struct PhotoEnvCache
    {
        unsigned int mirror_tex     = 0;
        unsigned int irradiance_tex = 0;
        // Fingerprint of the lights the current bake came from; a mismatch triggers a re-bake.
        // Baking is a few thousand CPU samples, far too slow to redo every frame just because the
        // camera moved, and far too stale to never redo when a light is dragged.
        std::array<float, 20> key{};
        bool valid = false;
        bool gl_objects_created() const { return mirror_tex != 0 && irradiance_tex != 0; }
    };
    PhotoEnvCache m_photo_env;
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

    // NEOTKO_PHOTOMODE_TAG s242: everything an external shadow RECEIVER needs to sample the map
    // this class owns. The Photo Mode cyclorama (GLCanvas3D::_render_photo_stage) is the only
    // caller: it is drawn after render_volumes_lit() has already filled the map, but it is not a
    // GLVolume, so it cannot be shaded by shells_lit and has to read the map itself.
    //
    // Handed over as one struct rather than five getters so a caller cannot pick up the matrix
    // and forget the validity flag — sampling an unfilled depth texture is the failure mode here,
    // and it looks like corruption rather than like a missing feature.
    struct ShadowMapHandle
    {
        bool     valid = false;
        unsigned int depth_tex = 0;
        int      res = 0;
        Matrix4d light_proj_view = Matrix4d::Identity();
        float    texel_world_mm = 0.0f;
    };
    ShadowMapHandle get_shadow_map_handle() const
    {
        ShadowMapHandle h;
        h.valid           = m_shadow_map_valid_this_frame && m_shadow_map_cache.gl_objects_created();
        h.depth_tex       = m_shadow_map_cache.depth_tex;
        h.res             = m_shadow_map_cache.res;
        h.light_proj_view = m_shadow_light_proj_view;
        h.texel_world_mm  = m_shadow_texel_world_mm;
        return h;
    }

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

    // NEOTKO_PHOTOMODE_TAG s242 (F5). Returns false if the probe is unavailable, in which case
    // render_volumes_lit() sends u_env_enabled=false and the shading falls back to the flat
    // hemisphere ambient — a duller picture, never a broken one.
    bool ensure_photo_env_textures();
    void destroy_photo_env_textures();
    // NEOTKO_REALCOLOR_TAG: debug-only ImGui panel exposing RealColorTuning as live sliders,
    // gated by NeoDebug::enabled(NeoDebug::REALCOLOR) — no-op unless ORCA_DEBUG_REALCOLOR is set
    void render_realcolor_debug_panel();
    // NEOTKO_REALCOLOR_TAG s251b: ventana de ACABADO POR FILAMENTO. A diferencia de
    // render_realcolor_debug_panel(), esto NO está detrás de NeoDebug::render_panels_enabled():
    // es una función de usuario, y se abre con un botón en la leyenda de la vista RealColor.
    //
    // 🚨 Y NO REBANA. El acabado es puramente visual: no llega al motor por ningún camino (sólo
    // alimenta el u_finish/u_aniso de realcolor_peel.fs). Eso lo separa del TD, que vive al lado en
    // app_config pero SÍ entra al slicer vía neotko_td_mirror y por eso su editor llama a
    // schedule_background_process(). Aquí eso sería un reslice gratis por mover un slider de brillo.
    void render_realcolor_materials_panel();
    bool m_realcolor_materials_panel_open = false;

    // NEOTKO_REALCOLOR_TAG s251e: TD editable EN VIVO desde la ventana de materiales, para poder
    // pseudo-calibrar mirando la pieza en vez de a ciegas. En unidades de RATIO (0.01-10), que es
    // como vive en app_config — NO en mm; a mm se hornea multiplicando por la altura de capa
    // nominal, igual que hace refresh_realcolor_materials().
    //
    // 🚨 POR QUÉ ESTO ES UN BUFFER APARTE Y NO SE ESCRIBE DIRECTO, al revés que el acabado. El
    // acabado es puramente visual; **el TD SÍ entra al motor** (app_config → neotko_td_mirror →
    // PrintApply), así que guardarlo en cada movimiento de slider dispararía un reslice por cada
    // pixel arrastrado. De ahí el botón de guardar explícito: mientras no se pulse, lo que se ve es
    // sólo el preview. Fue petición del usuario y es la decisión correcta — además hace VISIBLE la
    // diferencia entre "ajuste de vista" y "ajuste que toca la pieza", que es justo la distinción
    // que esta ventana tiene que enseñar.
    std::array<float, 4> m_realcolor_td_edit{ { 1.0f, 1.0f, 1.0f, 1.0f } };
    bool m_realcolor_td_dirty = false;
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

