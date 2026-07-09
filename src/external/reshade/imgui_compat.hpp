// Minimal ImGui type declarations required to unlock the ImGui:: namespace
// wrappers in reshade_overlay.hpp without vendoring the full imgui.h.
//
// Rules:
//   - Types used by VALUE in the function table are fully defined.
//   - Opaque types only used via pointer/reference are forward-declared.
//   - Flag types and IDs are typedef'd to their underlying primitive.
//   - Function pointer callback types are typedef'd with compatible (but
//     possibly simplified) signatures -- we never call them.
//
// This header must be included BEFORE reshade.hpp / reshade_overlay.hpp.

#pragma once

#define IMGUI_VERSION       "1.92.5"
#define IMGUI_VERSION_NUM   19250

#include <stdarg.h>    // va_list

// ── Primitive type aliases ───────────────────────────────────────────────────

typedef unsigned int   ImGuiID;
typedef unsigned int   ImU32;
typedef unsigned short ImWchar;
typedef unsigned long long ImU64;
typedef long long      ImS64;
typedef int            ImGuiWindowFlags;
typedef int            ImGuiChildFlags;
typedef int            ImGuiItemFlags;
typedef int            ImGuiFocusedFlags;
typedef int            ImGuiHoveredFlags;
typedef int            ImGuiInputTextFlags;
typedef int            ImGuiTreeNodeFlags;
typedef int            ImGuiPopupFlags;
typedef int            ImGuiSelectableFlags;
typedef int            ImGuiComboFlags;
typedef int            ImGuiTabBarFlags;
typedef int            ImGuiTabItemFlags;
typedef int            ImGuiTableFlags;
typedef int            ImGuiTableColumnFlags;
typedef int            ImGuiTableRowFlags;
typedef int            ImGuiTableBgTarget;
typedef int            ImGuiButtonFlags;
typedef int            ImGuiColorEditFlags;
typedef int            ImGuiDragDropFlags;
typedef int            ImGuiSliderFlags;
enum ImGuiSliderFlags_ {
    ImGuiSliderFlags_None            = 0,
    ImGuiSliderFlags_NoRoundToFormat = 1 << 4,
};
typedef int            ImGuiMouseButton;
typedef int            ImGuiMouseCursor;
typedef int            ImGuiSortDirection;
typedef int            ImGuiCond;
typedef int            ImGuiDataType;
typedef int            ImGuiDir;
typedef int            ImGuiKey;
typedef int            ImGuiKeyChord;
typedef int            ImGuiInputFlags;
typedef int            ImGuiMultiSelectFlags;
typedef int            ImGuiDockNodeFlags;
typedef int            ImGuiCol;
typedef int            ImGuiStyleVar;
typedef ImS64          ImGuiSelectionUserData;
typedef int            ImDrawFlags;
typedef int            ImDrawListFlags;
typedef int            ImFontAtlasFlags;
typedef int            ImGuiViewportFlags;
typedef int            ImGuiBackendFlags;
typedef int            ImGuiConfigFlags;
typedef ImU64          ImTextureID;

// ── Structs used by value ────────────────────────────────────────────────────

struct ImVec2 {
    float x, y;
    ImVec2() : x(0.0f), y(0.0f) {}
    ImVec2(float _x, float _y) : x(_x), y(_y) {}
};

struct ImVec4 {
    float x, y, z, w;
    ImVec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
    ImVec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
};

// ImTextureRef: thin wrapper around ImTextureID (added in ImGui ~1.92.x)
struct ImTextureRef {
    ImTextureID Data;
    ImTextureRef() : Data(0) {}
    ImTextureRef(ImTextureID id) : Data(id) {}
};

// ── Opaque structs (only used via pointer/reference; never dereferenced here) ─

struct ImGuiIO;
struct ImGuiStyle;
struct ImDrawListSharedData;
struct ImFontBaked;
struct ImFontAtlas;
struct ImGuiPayload;
struct ImGuiTableSortSpecs;
struct ImGuiTableColumnSortSpecs;
struct ImGuiViewport;
struct ImGuiWindowClass;
struct ImGuiMultiSelectIO;
struct ImGuiInputTextCallbackData;
struct ImGuiSizeCallbackData;

// ── Callback function pointer typedefs ───────────────────────────────────────

struct ImDrawList;   // forward-declared here; full decl below
typedef void  (*ImDrawCallback)(const ImDrawList* parent_list, const void* cmd);
typedef int   (*ImGuiInputTextCallback)(ImGuiInputTextCallbackData* data);
typedef void  (*ImGuiSizeCallback)(ImGuiSizeCallbackData* data);
typedef void* (*ImGuiMemAllocFunc)(size_t sz, void* user_data);
typedef void  (*ImGuiMemFreeFunc)(void* ptr, void* user_data);

// ── ImGuiStorage ─────────────────────────────────────────────────────────────
// reshade_overlay.hpp defines inline methods on this class (lines 821-834).

struct ImGuiStorage {
    int    GetInt    (ImGuiID key, int   default_val = 0)   const;
    void   SetInt    (ImGuiID key, int   val);
    bool   GetBool   (ImGuiID key, bool  default_val = false) const;
    void   SetBool   (ImGuiID key, bool  val);
    float  GetFloat  (ImGuiID key, float default_val = 0.0f) const;
    void   SetFloat  (ImGuiID key, float val);
    void*  GetVoidPtr(ImGuiID key) const;
    void   SetVoidPtr(ImGuiID key, void* val);
    int*   GetIntRef    (ImGuiID key, int   default_val = 0);
    bool*  GetBoolRef   (ImGuiID key, bool  default_val = false);
    float* GetFloatRef  (ImGuiID key, float default_val = 0.0f);
    void** GetVoidPtrRef(ImGuiID key, void* default_val = nullptr);
    void   BuildSortByKey();
    void   SetAllInt(int val);
};

// ── ImGuiListClipper ──────────────────────────────────────────────────────────
// reshade_overlay.hpp defines inline ctor/dtor and methods (lines 835-841).

struct ImGuiListClipper {
    ImGuiListClipper();
    ~ImGuiListClipper();
    void Begin(int items_count, float items_height = -1.0f);
    void End();
    bool Step();
    void IncludeItemsByIndex(int item_begin, int item_end);
    void SeekCursorForItem(int item_index);
};

// ── ImFont ────────────────────────────────────────────────────────────────────
// reshade_overlay.hpp defines inline ctor/dtor and IsGlyphInFont (lines 887-889).

struct ImFont {
    ImFont();
    ~ImFont();
    bool IsGlyphInFont(ImWchar c);
};

// ── ImDrawList ────────────────────────────────────────────────────────────────
// reshade_overlay.hpp defines a large set of inline methods (lines 842-886).
// Every method used in those inline bodies must be declared here.

struct ImDrawList {
    explicit ImDrawList(ImDrawListSharedData* shared_data);
    ~ImDrawList();

    void PushClipRect(const ImVec2& clip_rect_min, const ImVec2& clip_rect_max, bool intersect_with_current_clip_rect);
    void PushClipRectFullScreen();
    void PopClipRect();
    void PushTexture(ImTextureRef tex_ref);
    void PopTexture();

    void AddLine(const ImVec2& p1, const ImVec2& p2, ImU32 col, float thickness = 1.0f);
    void AddRect(const ImVec2& p_min, const ImVec2& p_max, ImU32 col, float rounding, ImDrawFlags flags, float thickness);
    void AddRectFilled(const ImVec2& p_min, const ImVec2& p_max, ImU32 col, float rounding = 0.0f, ImDrawFlags flags = 0);
    void AddRectFilledMultiColor(const ImVec2& p_min, const ImVec2& p_max, ImU32 col_upr_left, ImU32 col_upr_right, ImU32 col_bot_right, ImU32 col_bot_left);
    void AddQuad(const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, const ImVec2& p4, ImU32 col, float thickness = 1.0f);
    void AddQuadFilled(const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, const ImVec2& p4, ImU32 col);
    void AddTriangle(const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, ImU32 col, float thickness = 1.0f);
    void AddTriangleFilled(const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, ImU32 col);
    void AddCircle(const ImVec2& center, float radius, ImU32 col, int num_segments = 0, float thickness = 1.0f);
    void AddCircleFilled(const ImVec2& center, float radius, ImU32 col, int num_segments = 0);
    void AddNgon(const ImVec2& center, float radius, ImU32 col, int num_segments, float thickness = 1.0f);
    void AddNgonFilled(const ImVec2& center, float radius, ImU32 col, int num_segments);
    void AddEllipse(const ImVec2& center, const ImVec2& radius, ImU32 col, float rot = 0.0f, int num_segments = 0, float thickness = 1.0f);
    void AddEllipseFilled(const ImVec2& center, const ImVec2& radius, ImU32 col, float rot = 0.0f, int num_segments = 0);
    void AddText(const ImVec2& pos, ImU32 col, const char* text_begin, const char* text_end = nullptr);
    void AddText(ImFont* font, float font_size, const ImVec2& pos, ImU32 col, const char* text_begin, const char* text_end = nullptr, float wrap_width = 0.0f, const ImVec4* cpu_fine_clip_rect = nullptr);
    void AddBezierCubic(const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, const ImVec2& p4, ImU32 col, float thickness, int num_segments = 0);
    void AddBezierQuadratic(const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, ImU32 col, float thickness, int num_segments = 0);
    void AddPolyline(const ImVec2* points, int num_points, ImU32 col, ImDrawFlags flags, float thickness);
    void AddConvexPolyFilled(const ImVec2* points, int num_points, ImU32 col);
    void AddConcavePolyFilled(const ImVec2* points, int num_points, ImU32 col);
    void AddImage(ImTextureRef tex_ref, const ImVec2& p_min, const ImVec2& p_max, const ImVec2& uv_min, const ImVec2& uv_max, ImU32 col);
    void AddImageQuad(ImTextureRef tex_ref, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, const ImVec2& p4, const ImVec2& uv1, const ImVec2& uv2, const ImVec2& uv3, const ImVec2& uv4, ImU32 col);
    void AddImageRounded(ImTextureRef tex_ref, const ImVec2& p_min, const ImVec2& p_max, const ImVec2& uv_min, const ImVec2& uv_max, ImU32 col, float rounding, ImDrawFlags flags);
    void PathArcTo(const ImVec2& center, float radius, float a_min, float a_max, int num_segments = 0);
    void PathArcToFast(const ImVec2& center, float radius, int a_min_of_12, int a_max_of_12);
    void PathEllipticalArcTo(const ImVec2& center, const ImVec2& radius, float rot, float a_min, float a_max, int num_segments = 0);
    void PathBezierCubicCurveTo(const ImVec2& p2, const ImVec2& p3, const ImVec2& p4, int num_segments = 0);
    void PathBezierQuadraticCurveTo(const ImVec2& p2, const ImVec2& p3, int num_segments = 0);
    void PathRect(const ImVec2& rect_min, const ImVec2& rect_max, float rounding = 0.0f, ImDrawFlags flags = 0);
    void AddCallback(ImDrawCallback callback, void* userdata, size_t userdata_size = 0);
    void AddDrawCmd();
    ImDrawList* CloneOutput() const;
    void PrimReserve(int idx_count, int vtx_count);
    void PrimUnreserve(int idx_count, int vtx_count);
    void PrimRect(const ImVec2& a, const ImVec2& b, ImU32 col);
    void PrimRectUV(const ImVec2& a, const ImVec2& b, const ImVec2& uv_a, const ImVec2& uv_b, ImU32 col);
    void PrimQuadUV(const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d, const ImVec2& uv_a, const ImVec2& uv_b, const ImVec2& uv_c, const ImVec2& uv_d, ImU32 col);
};
