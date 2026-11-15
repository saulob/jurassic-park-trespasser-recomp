/* The Graphics card setting: a DXGI adapter by its label, the first usable adapter when no label matches. */
#ifndef JPT_GPU_H
#define JPT_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_GPU_MAX 16
#define JPT_GPU_NAME_MAX 128
#define JPT_GPU_LABEL_MAX 144

typedef struct {
    char name[JPT_GPU_NAME_MAX];   /* the adapter's description; empty: an adapter the renderer cannot use */
    char label[JPT_GPU_LABEL_MAX]; /* what the launcher shows and graphics_card stores: the name, " (2)", " (3)" for repeats */
} jpt_gpu_t;

/* The renderer's D3D_FEATURE_LEVEL values, best first. */
#define JPT_GPU_FEATURE_LEVEL_COUNT 5
extern const int jpt_gpu_feature_levels[JPT_GPU_FEATURE_LEVEL_COUNT];

/* The DXGI adapters in enumeration order: entry i is EnumAdapters1(i). Software adapters and adapters Direct3D 11
 * cannot create a device on are listed with an empty name. Returns the count, labels assigned. */
int jpt_gpu_list(jpt_gpu_t* out, int cap);
/* Fills every label from the names: the first adapter of a name keeps it, later ones get " (2)", " (3)". */
void jpt_gpu_assign_labels(jpt_gpu_t* gpus, int count);
/* The index of the usable adapter labelled exactly `request`; -1 when the request is empty or matches none. */
int jpt_gpu_find(const jpt_gpu_t* gpus, int count, const char* request);
/* The adapter labelled `request`, else the first usable one; -1 only when no adapter is usable. */
int jpt_gpu_resolve(const jpt_gpu_t* gpus, int count, const char* request);

#ifdef __cplusplus
}
#endif

#endif
