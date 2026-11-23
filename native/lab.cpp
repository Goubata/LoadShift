#include <atomic>
// On-device lab for LoadShift hooks (runs as a plain native process, no Zygisk).
// usage: lab <gles|vk> <libloadshift.so> <conf-file> <data-dir>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "lab_spv.h"

static double ms(clockid_t c = CLOCK_MONOTONIC) {
    timespec t;
    clock_gettime(c, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static const char *(*lab_install)(const char *, const char *, const char *);
static void (*lab_flush)();

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return strdup("");
    static char b[4096];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static GLuint prog(const char *vs, const char *fs) {
    auto sh = [](GLenum t, const char *s) {
        GLuint o = glCreateShader(t);
        glShaderSource(o, 1, &s, nullptr);
        glCompileShader(o);
        return o;
    };
    GLuint p = glCreateProgram();
    glAttachShader(p, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

static int gles(const char *conf, const char *dir) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, nullptr, nullptr);
    printf("hooks: %s\n", lab_install(conf, dir, "lab.gles"));
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    printf("GL: %s\n", glGetString(GL_VERSION));

    // --- 1. GPU-heavy frames + glFinish: how long is the CPU blocked?
    const int W = 1920, H = 1080;
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    GLuint p = prog("#version 300 es\nvoid main(){vec2 v=vec2(gl_VertexID==1?3.:-1.,gl_VertexID==2?3.:-1.);gl_Position=vec4(v,0,1);}",
                    "#version 300 es\nprecision highp float;out vec4 o;uniform float t;void main(){vec2 u=gl_FragCoord.xy;float a=t;"
                    "for(int i=0;i<160;i++){a=sin(a+u.x*0.001)*cos(a-u.y*0.001)+a*0.5;}o=vec4(a,a*.5,1.-a,1);}");
    glUseProgram(p);
    GLint ut = glGetUniformLocation(p, "t");
    glViewport(0, 0, W, H);
    double blocked = 0, t0 = ms();
    const int frames = 60;
    for (int f = 0; f < frames; f++) {
        glUniform1f(ut, f * 0.01f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        double b0 = ms();
        glFinish();
        blocked += ms() - b0;
    }
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t[4]){});  // drains everything
    double total = ms() - t0;
    printf("finish: %d frames total %.1f ms, CPU blocked in glFinish %.1f ms (%.2f ms/frame)\n", frames, total, blocked, blocked / frames);

    // --- 1b. same, with ~6 ms of simulated game logic on the CPU each frame
    blocked = 0; t0 = ms();
    for (int f = 0; f < frames; f++) {
        double s0 = ms(); volatile double acc = 0; while (ms() - s0 < 6.0) acc += 1;
        glUniform1f(ut, f * 0.01f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        double b0 = ms();
        glFinish();
        blocked += ms() - b0;
    }
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t[4]){});
    total = ms() - t0;
    printf("finish+cpu6ms: %d frames total %.1f ms -> %.1f fps, blocked %.2f ms/frame\n", frames, total, frames * 1000.0 / total, blocked / frames);

    // --- 2. texture uploads: CPU time on the calling thread + correctness
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const int TW = 2048, TH = 2048;
    uint8_t *img = (uint8_t *)malloc(TW * TH * 4);
    for (int i = 0; i < TW * TH * 4; i++) img[i] = (uint8_t)(i * 2654435761u >> 24);
    GLuint tex;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TW, TH);
    glFinish();
    double c0 = ms(CLOCK_THREAD_CPUTIME_ID), w0 = ms();
    const int ups = 30;
    for (int i = 0; i < ups; i++) {
        img[i] ^= 0xff;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, img);
    }
    double callCpu = ms(CLOCK_THREAD_CPUTIME_ID) - c0, callWall = ms() - w0;
    glFinish();
    printf("upload: %d x 2048^2 RGBA, calls took %.1f ms wall / %.1f ms CPU, until done %.1f ms\n", ups, callWall, callCpu, ms() - w0);

    // correctness: odd size RGB with alignment 1, row length + skip
    const int RW = 1023, RH = 517, ROWLEN = 1100, SKR = 3, SKP = 5;
    uint8_t *rgb = (uint8_t *)malloc(ROWLEN * (RH + SKR) * 3);
    for (int i = 0; i < ROWLEN * (RH + SKR) * 3; i++) rgb[i] = (uint8_t)(i * 7 + (i >> 9));
    GLuint t2; glGenTextures(1, &t2); glBindTexture(GL_TEXTURE_2D, t2);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, ROWLEN);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, SKR);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, SKP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, RW, RH, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLuint f2; glGenFramebuffers(1, &f2); glBindFramebuffer(GL_FRAMEBUFFER, f2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t2, 0);
    uint8_t *back = (uint8_t *)malloc(RW * RH * 4);
    glReadPixels(0, 0, RW, RH, GL_RGBA, GL_UNSIGNED_BYTE, back);
    long bad = 0;
    for (int y = 0; y < RH; y++)
        for (int xx = 0; xx < RW; xx++)
            for (int k = 0; k < 3; k++)
                if (back[(y * RW + xx) * 4 + k] != rgb[((y + SKR) * ROWLEN + xx + SKP) * 3 + k]) bad++;
    printf("upload-correctness: RGB 1023x517 rowlen/skip/align1 mismatches=%ld err=0x%x\n", bad, glGetError());
    lab_flush();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
    return bad ? 1 : 0;
}

static int vk() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst)) { puts("no instance"); return 2; }
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueCount = 1; q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, nullptr, &dev)) { puts("no device"); return 2; }
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = sizeof(lab_comp_spv); sci.pCode = lab_comp_spv;
    VkShaderModule sm; vkCreateShaderModule(dev, &sci, nullptr, &sm);
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1; dl.pBindings = &b;
    VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &dsl;
    VkPipelineLayout layout; vkCreatePipelineLayout(dev, &pl, nullptr, &layout);
    // LAB_APPCACHE=1: behave like Unreal — the app owns a pipeline cache created WITH its own
    // initial data (here: a valid header-only blob), never VK_NULL_HANDLE.
    VkPipelineCache appCache = VK_NULL_HANDLE;
    if (getenv("LAB_APPCACHE")) {
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache empty; vkCreatePipelineCache(dev, &ci, nullptr, &empty);
        size_t sz = 0; vkGetPipelineCacheData(dev, empty, &sz, nullptr);
        static uint8_t hdr[4096]; if (sz > sizeof(hdr)) sz = sizeof(hdr);
        vkGetPipelineCacheData(dev, empty, &sz, hdr);
        vkDestroyPipelineCache(dev, empty, nullptr);
        ci.initialDataSize = sz; ci.pInitialData = hdr;
        vkCreatePipelineCache(dev, &ci, nullptr, &appCache);
        printf("app-owned cache with %zu bytes of its own initial data\n", sz);
    }
    const int N = 48;
    double total = 0;
    for (int i = 0; i < N; i++) {
        int32_t spec[2] = {i * 37 + 11, i % 7 + 3};
        VkSpecializationMapEntry me[2] = {{0, 0, 4}, {1, 4, 4}};
        VkSpecializationInfo si{2, me, sizeof(spec), spec};
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", &si};
        cp.layout = layout;
        VkPipeline pipe;
        double t0 = ms();
        VkResult r = vkCreateComputePipelines(dev, appCache, 1, &cp, nullptr, &pipe);
        total += ms() - t0;
        if (r) { printf("pipeline %d failed %d\n", i, r); return 1; }
        vkDestroyPipeline(dev, pipe, nullptr);
    }
    printf("vk: %d compute pipelines (%s) %.1f ms total, %.2f ms avg\n", N, appCache ? "app cache" : "null cache", total, total / N);
    vkDestroyDevice(dev, nullptr);  // hook saves the implicit cache here
    vkDestroyInstance(inst, nullptr);
    lab_flush();
    return 0;
}


static void spin(double msec) { double s0 = ms(); volatile double acc = 0; while (ms() - s0 < msec) acc += 1; }

static int glbuf_test(const char *conf, const char *dir) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, nullptr, nullptr);
    printf("hooks: %s\n", lab_install(conf, dir, "lab.glbuf"));
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    const int W = 1920, H = 1080;
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, W, H);
    // 8 MiB vertex buffer (vec4 per vertex) that every draw reads.
    const int NV = 8 * 1024 * 1024 / 16;
    float *mirror = (float *)malloc(NV * 16);
    for (int i = 0; i < NV * 4; i++) mirror[i] = (float)((i * 7) % 1000) * 0.001f;
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, NV * 16, mirror, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
    GLuint p = prog("#version 300 es\nlayout(location=0) in vec4 a;out float v;void main(){float t=a.x;for(int i=0;i<24;i++)t=sin(t*1.3+a.y);v=t;"
                    "gl_Position=vec4(fract(a.x*7.)*2.-1.,fract(a.y*3.)*2.-1.,0,1);gl_PointSize=2.;}",
                    "#version 300 es\nprecision highp float;in float v;out vec4 o;void main(){float t=v;for(int i=0;i<32;i++)t=cos(t+gl_FragCoord.x*0.001);o=vec4(t);}");
    glUseProgram(p);
    const int frames = 40, CH = 1 << 20;
    uint8_t *src = (uint8_t *)malloc(CH);
    const char *mode[3] = {"BufferSubData 1MiB", "Map INVALIDATE 1MiB", "Map FLUSH_EXPLICIT 2x64KiB"};
    for (int m = 0; m < 3; m++) {
        glFinish();
        double inCall = 0, t0 = ms();
        for (int f = 0; f < frames; f++) {
            glDrawArrays(GL_POINTS, 0, NV);         // GPU reads the whole buffer
            spin(4.0);                               // game logic
            size_t off = (size_t)((f * 2654435761u) % (NV * 16 / CH - 1)) * CH;
            for (int k = 0; k < CH / 4; k++) ((float *)src)[k] = (float)(f * 31 + k % 977 + m) * 0.001f;
            double c0 = ms();
            if (m == 0) {
                glBufferSubData(GL_ARRAY_BUFFER, off, CH, src);
                memcpy((uint8_t *)mirror + off, src, CH);
            } else if (m == 1) {
                void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, off, CH, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
                memcpy(ptr, src, CH);
                glUnmapBuffer(GL_ARRAY_BUFFER);
                memcpy((uint8_t *)mirror + off, src, CH);
            } else {
                void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, off, CH, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
                memcpy((uint8_t *)ptr + 4096, src, 65536);
                memcpy((uint8_t *)ptr + 600000, src + 1000, 65536);
                glFlushMappedBufferRange(GL_ARRAY_BUFFER, 4096, 65536);
                glFlushMappedBufferRange(GL_ARRAY_BUFFER, 600000, 65536);
                glUnmapBuffer(GL_ARRAY_BUFFER);
                memcpy((uint8_t *)mirror + off + 4096, src, 65536);
                memcpy((uint8_t *)mirror + off + 600000, src + 1000, 65536);
            }
            inCall += ms() - c0;
            glFlush();
        }
        glFinish();
        double total = ms() - t0;
        printf("%-28s %d frames %.1f ms -> %.1f fps, CPU in update call %.2f ms/frame\n", mode[m], frames, total, frames * 1000.0 / total, inCall / frames);
    }
    // correctness: read back the whole buffer
    void *rb = glMapBufferRange(GL_ARRAY_BUFFER, 0, NV * 16, GL_MAP_READ_BIT);
    long bad = 0;
    for (int i = 0; i < NV * 4; i++) if (((float *)rb)[i] != mirror[i]) bad++;
    glUnmapBuffer(GL_ARRAY_BUFFER);
    printf("glbuf-correctness: mismatched floats=%ld err=0x%x\n", bad, glGetError());
    lab_flush();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
    return bad ? 1 : 0;
}

static int vkidle_test() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst)) return 2;
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueCount = 1; q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, nullptr, &dev)) return 2;
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    const uint32_t COUNT = 64 * 2048;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = COUNT * 4; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buf; vkCreateBuffer(dev, &bi, nullptr, &buf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = type;
    VkDeviceMemory mem; vkAllocateMemory(dev, &mai, nullptr, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = sizeof(idle_comp_spv); sci.pCode = idle_comp_spv;
    VkShaderModule sm; vkCreateShaderModule(dev, &sci, nullptr, &sm);
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1; dl.pBindings = &b;
    VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    VkPipelineLayout layout; vkCreatePipelineLayout(dev, &pl, nullptr, &layout);
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cp.layout = layout;
    VkPipeline pipe; vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VkDescriptorPool dp; vkCreateDescriptorPool(dev, &dpi, nullptr, &dp);
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = dp; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &dsl;
    VkDescriptorSet set; vkAllocateDescriptorSets(dev, &dsa, &set);
    VkDescriptorBufferInfo dbi{buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool; vkCreateCommandPool(dev, &cpi, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cai, &cb);

    int iters = getenv("LAB_ITERS") ? atoi(getenv("LAB_ITERS")) : 600;
    const int frames = 60;
    long bad = 0;
    double idleCall = 0, t0 = ms();
    for (int f = 1; f <= frames; f++) {
        VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bb);                 // reuse: must wait for previous run
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        int pc[2] = {f, iters};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        vkCmdDispatch(cb, COUNT / 64, 1, 1);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        double c0 = ms();
        vkQueueWaitIdle(queue);                         // the classic stall
        idleCall += ms() - c0;
        // per-frame transient object, destroyed right after "idle"
        VkBuffer tmp; vkCreateBuffer(dev, &bi, nullptr, &tmp); vkDestroyBuffer(dev, tmp, nullptr);
        spin(6.0);                                      // game logic
        if (f % 10 == 0) {                              // CPU readback must see THIS frame
            void *ptr; vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr);
            for (uint32_t i = 0; i < COUNT; i++) if (((int *)ptr)[i] != f * 1000 + (int)(i % 1000)) bad++;
            vkUnmapMemory(dev, mem);
        }
    }
    double total = ms() - t0;
    printf("vkidle: %d frames %.1f ms -> %.1f fps, CPU in vkQueueWaitIdle %.2f ms/frame, readback mismatches=%ld\n",
           frames, total, frames * 1000.0 / total, idleCall / frames, bad);
    vkDeviceWaitIdle(dev);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    lab_flush();
    return bad ? 1 : 0;
}

// Measures where the driver burns CPU on the calling thread (baseline exploration).
static int probe_test(const char *conf, const char *dir) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, nullptr, nullptr);
    printf("hooks: %s\n", lab_install(conf, dir, "lab.probe"));
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    const int W = 1920, H = 1080;
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, W, H);
    GLuint p = prog("#version 300 es\nvoid main(){vec2 v=vec2(gl_VertexID==1?3.:-1.,gl_VertexID==2?3.:-1.);gl_Position=vec4(v,0,1);}",
                    "#version 300 es\nprecision highp float;out vec4 o;uniform sampler2D t;void main(){vec4 a=vec4(0);"
                    "for(int i=0;i<24;i++)a+=texture(t,gl_FragCoord.xy/vec2(1920.,1080.)+float(i)*0.001);o=a;}");
    glUseProgram(p);
    const int frames = 30;
    auto cpu = [] { return ms(CLOCK_THREAD_CPUTIME_ID); };
    // 1) glBufferData same size every frame on a buffer the GPU reads
    {
        GLuint b; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b);
        const int SZ = 1 << 20; uint8_t *data = (uint8_t *)calloc(SZ, 1);
        glBufferData(GL_ARRAY_BUFFER, SZ, data, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0); glEnableVertexAttribArray(0);
        double w = 0, cc = 0;
        for (int f = 0; f < frames; f++) {
            glDrawArrays(GL_TRIANGLES, 0, 3);
            data[f] = f;
            double w0 = ms(), c0 = cpu();
            glBufferData(GL_ARRAY_BUFFER, SZ, data, GL_DYNAMIC_DRAW);
            w += ms() - w0; cc += cpu() - c0;
            glFlush();
        }
        glFinish();
        printf("BufferData respec 1MiB in-use : %.3f ms wall, %.3f ms cpu per call\n", w / frames, cc / frames);
        glDisableVertexAttribArray(0);
    }
    // 2) glTexSubImage2D on a texture sampled by the pending draw
    {
        const int TW = 1024; uint8_t *img = (uint8_t *)malloc(TW * TW * 4);
        memset(img, 7, TW * TW * 4);
        GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TW, TW);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        double w = 0, cc = 0;
        for (int f = 0; f < frames; f++) {
            glDrawArrays(GL_TRIANGLES, 0, 3);
            img[f] = f;
            double w0 = ms(), c0 = cpu();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, TW, GL_RGBA, GL_UNSIGNED_BYTE, img);
            w += ms() - w0; cc += cpu() - c0;
            glFlush();
        }
        glFinish();
        printf("TexSubImage 1024^2 RGBA in-use: %.3f ms wall, %.3f ms cpu per call\n", w / frames, cc / frames);
    }
    // 3) format conversion cost: same pixel count, idle texture
    struct F { const char *name; GLenum ifmt, fmt, type; int bpp; } fm[] = {
        {"RGBA8", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}, {"RGB8 ", GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3},
        {"LUM_A", GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2}, {"RGBA4", GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2},
        {"R8   ", GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1}, {"RGB565", GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2},
    };
    const int TW = 2048; uint8_t *img = (uint8_t *)malloc(TW * TW * 4);
    for (int i = 0; i < TW * TW * 4; i++) img[i] = (uint8_t)(i * 13);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (auto &q : fm) {
        GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        if (q.ifmt == GL_LUMINANCE_ALPHA) glTexImage2D(GL_TEXTURE_2D, 0, q.ifmt, TW, TW, 0, q.fmt, q.type, nullptr);
        else glTexStorage2D(GL_TEXTURE_2D, 1, q.ifmt, TW, TW);
        glFinish();
        double w = 0, cc = 0;
        for (int f = 0; f < 10; f++) {
            double w0 = ms(), c0 = cpu();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, TW, q.fmt, q.type, img);
            w += ms() - w0; cc += cpu() - c0;
        }
        glFinish();
        printf("upload 2048^2 %s : %.2f ms wall, %.2f ms cpu per call (%.0f MB/s)\n", q.name, w / 10, cc / 10, TW * TW * q.bpp / 1048576.0 / (w / 10000));
        glDeleteTextures(1, &t);
    }
    // 4) compressed (ETC2 RGBA8, 4 MiB per 2048^2 level)
    {
        GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_RGBA8_ETC2_EAC, TW, TW);
        glFinish();
        int sz = TW * TW;  // 1 byte/pixel
        double w = 0, cc = 0;
        for (int f = 0; f < 10; f++) {
            double w0 = ms(), c0 = cpu();
            glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TW, TW, GL_COMPRESSED_RGBA8_ETC2_EAC, sz, img);
            w += ms() - w0; cc += cpu() - c0;
        }
        glFinish();
        printf("upload 2048^2 ETC2  : %.2f ms wall, %.2f ms cpu per call, err=0x%x\n", w / 10, cc / 10, glGetError());
    }
    lab_flush();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
    return 0;
}

static int occl_gl_test(const char *conf, const char *dir) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, nullptr, nullptr);
    printf("hooks: %s\n", lab_install(conf, dir, "lab.occl"));
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    const int W = 1920, H = 1080;
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, W, H);
    GLuint p = prog("#version 300 es\nvoid main(){vec2 v=vec2(gl_VertexID==1?3.:-1.,gl_VertexID==2?3.:-1.);gl_Position=vec4(v,0,1);}",
                    "#version 300 es\nprecision highp float;out vec4 o;void main(){float a=gl_FragCoord.x*0.001;for(int i=0;i<200;i++)a=sin(a*1.1+gl_FragCoord.y*0.001);o=vec4(a);}");
    glUseProgram(p);
    GLuint q[2]; glGenQueries(2, q);
    const int frames = 40;
    long wrong = 0;
    double inCall = 0, t0 = ms();
    for (int f = 0; f < frames; f++) {
        glBeginQuery(GL_ANY_SAMPLES_PASSED, q[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);          // visible
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        double c0 = ms();
        GLuint r = 7;
        glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &r);   // engines that read immediately stall here
        inCall += ms() - c0;
        if (r != 1) wrong++;                        // visible draw must never read as hidden
        spin(4.0);
    }
    glFinish();
    double total = ms() - t0;
    // an occluded query that has completed must report the true 0
    glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 0, 0);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, q[1]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    glFinish();
    GLuint hidden = 7; glGetQueryObjectuiv(q[1], GL_QUERY_RESULT, &hidden);
    printf("occl-gl: %d frames %.1f ms -> %.1f fps, CPU in GetQueryObject %.2f ms/frame, visible-wrong=%ld, completed-hidden=%u (expect 0)\n",
           frames, total, frames * 1000.0 / total, inCall / frames, wrong, hidden);
    lab_flush();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
    return (wrong || hidden != 0) ? 1 : 0;
}

static int occl_vk_test() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    VkInstance inst; if (vkCreateInstance(&ici, nullptr, &inst)) return 2;
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    VkDevice dev; if (vkCreateDevice(pd, &dci, nullptr, &dev)) return 2;
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    const uint32_t W = 1920, H = 1080;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = {W, H, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage img; vkCreateImage(dev, &ii, nullptr, &img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = 0; for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = type;
    VkDeviceMemory mem; vkAllocateMemory(dev, &mai, nullptr, &mem); vkBindImageMemory(dev, img, mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view; vkCreateImageView(dev, &vi, nullptr, &view);
    VkAttachmentDescription ad{0, ii.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                               VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd{}; sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 1; rpi.pAttachments = &ad; rpi.subpassCount = 1; rpi.pSubpasses = &sd;
    VkRenderPass rp; vkCreateRenderPass(dev, &rpi, nullptr, &rp);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &view; fi.width = W; fi.height = H; fi.layers = 1;
    VkFramebuffer fb; vkCreateFramebuffer(dev, &fi, nullptr, &fb);
    VkShaderModuleCreateInfo vs{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; vs.codeSize = sizeof(occl_vert_spv); vs.pCode = occl_vert_spv;
    VkShaderModuleCreateInfo fs{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; fs.codeSize = sizeof(occl_frag_spv); fs.pCode = occl_frag_spv;
    VkShaderModule vm, fm; vkCreateShaderModule(dev, &vs, nullptr, &vm); vkCreateShaderModule(dev, &fs, nullptr, &fm);
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VkPipelineLayout layout; vkCreatePipelineLayout(dev, &pli, nullptr, &layout);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main"}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, (float)W, (float)H, 0, 1}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1; rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms_{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb_{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb_.attachmentCount = 1; cb_.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms_; gp.pColorBlendState = &cb_; gp.layout = layout; gp.renderPass = rp;
    VkPipeline pipe; if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe)) { puts("pipeline failed"); return 2; }
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qpi.queryType = VK_QUERY_TYPE_OCCLUSION; qpi.queryCount = 2;
    VkQueryPool pool; vkCreateQueryPool(dev, &qpi, nullptr, &pool);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cpool; vkCreateCommandPool(dev, &cpi, nullptr, &cpool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = cpool; cai.commandBufferCount = 2;
    VkCommandBuffer cbs[2]; vkAllocateCommandBuffers(dev, &cai, cbs);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fences[2]; vkCreateFence(dev, &fci, nullptr, &fences[0]); vkCreateFence(dev, &fci, nullptr, &fences[1]);
    int iters = getenv("LAB_ITERS") ? atoi(getenv("LAB_ITERS")) : 200;
    const int frames = 40;
    long wrong = 0;
    double inCall = 0, t0 = ms();
    for (int f = 0; f < frames; f++) {
        int k = f & 1;
        vkWaitForFences(dev, 1, &fences[k], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fences[k]);
        VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[k], &bb);
        vkCmdResetQueryPool(cbs[k], pool, k, 1);
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = sc;
        vkCmdBeginRenderPass(cbs[k], &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cbs[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(cbs[k], layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &iters);
        vkCmdBeginQuery(cbs[k], pool, k, 0);
        vkCmdDraw(cbs[k], 3, 1, 0, 0);
        vkCmdEndQuery(cbs[k], pool, k);
        vkCmdEndRenderPass(cbs[k]);
        vkEndCommandBuffer(cbs[k]);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cbs[k];
        vkQueueSubmit(queue, 1, &si, fences[k]);
        double c0 = ms();
        uint64_t r[2] = {0, 0};
        vkGetQueryPoolResults(dev, pool, k, 1, sizeof(r), r, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        inCall += ms() - c0;
        if (r[0] == 0) wrong++;
        spin(4.0);
    }
    vkWaitForFences(dev, 2, fences, VK_TRUE, UINT64_MAX);
    double total = ms() - t0;
    uint64_t final[2] = {0, 0};
    vkGetQueryPoolResults(dev, pool, (frames - 1) & 1, 1, 16, final, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    printf("occl-vk: %d frames %.1f ms -> %.1f fps, CPU in GetQueryPoolResults %.2f ms/frame, visible-wrong=%ld, completed-samples=%llu\n",
           frames, total, frames * 1000.0 / total, inCall / frames, wrong, (unsigned long long)final[0]);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    lab_flush();
    return (wrong || final[0] < (uint64_t)W * H / 2) ? 1 : 0;
}

static int probe2_test(const char *conf, const char *dir) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, nullptr, nullptr);
    printf("hooks: %s\n", lab_install(conf, dir, "lab.probe2"));
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    const int W = 1920, H = 1080;
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, W, H);
    GLuint p = prog("#version 300 es\nvoid main(){vec2 v=vec2(gl_VertexID==1?3.:-1.,gl_VertexID==2?3.:-1.);gl_Position=vec4(v,0,1);}",
                    "#version 300 es\nprecision highp float;out vec4 o;uniform float k;void main(){float a=gl_FragCoord.x*0.001+k;for(int i=0;i<120;i++)a=sin(a*1.1+gl_FragCoord.y*0.001);o=vec4(a,k,0.5,1);}");
    glUseProgram(p);
    GLint uk = glGetUniformLocation(p, "k");
    const int frames = 40;
    // 1) per-frame small readback (auto-exposure / picking style) right after rendering
    double inCall = 0, t0 = ms();
    uint8_t px[64 * 64 * 4];
    for (int f = 0; f < frames; f++) {
        glUniform1f(uk, (f % 10) * 0.1f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        double c0 = ms();
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
        inCall += ms() - c0;
        spin(4.0);
    }
    double total = ms() - t0;
    printf("ReadPixels 64x64 per frame: %.1f fps, CPU in call %.2f ms/frame, last G=%d (expect ~%d)\n", frames * 1000.0 / total, inCall / frames, px[1], (int)(((frames - 1) % 10) * 0.1f * 255 + 0.5f));
    // 2) cost of glFlush and glGetError
    glFinish();
    double c0 = ms();
    for (int i = 0; i < 2000; i++) { glUniform1f(uk, i * 1e-4f); glDrawArrays(GL_TRIANGLES, 0, 0); glFlush(); }
    double fl = ms() - c0;
    glFinish();
    c0 = ms();
    for (int i = 0; i < 2000; i++) { glUniform1f(uk, i * 1e-4f); glDrawArrays(GL_TRIANGLES, 0, 0); }
    double nofl = ms() - c0;
    glFinish();
    c0 = ms();
    for (int i = 0; i < 20000; i++) glGetError();
    double ge = ms() - c0;
    printf("glFlush: %.1f us each (with vs without), glGetError: %.2f us each\n", (fl - nofl) * 1000 / 2000, ge * 1000 / 20000);
    // 3) flush with real tiny draws (what games do between passes)
    glFinish();
    c0 = ms();
    for (int i = 0; i < 300; i++) { glViewport(0, 0, 8, 8); glDrawArrays(GL_TRIANGLES, 0, 3); glFlush(); }
    double fl2 = ms() - c0;
    glFinish();
    c0 = ms();
    for (int i = 0; i < 300; i++) { glViewport(0, 0, 8, 8); glDrawArrays(GL_TRIANGLES, 0, 3); }
    double nofl2 = ms() - c0;
    glFinish();
    printf("glFlush after real draw: %.1f us each\n", (fl2 - nofl2) * 1000 / 300);
    lab_flush();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
    return 0;
}

// ---- coexistence test: emulate another in-process graphics hook module -----------------------
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static std::atomic<int> f_finish{0}, f_bsd{0}, f_vkidle{0}, f_vkdestroy{0};
static void (*fo_finish)();
static void (*fo_bsd)(GLenum, GLintptr, GLsizeiptr, const void *);
static VkResult (*fo_vkidle)(VkQueue);
static void (*fo_vkdestroy)(VkDevice, VkBuffer, const VkAllocationCallbacks *);
static void fh_finish() { f_finish++; fo_finish(); }
static void fh_bsd(GLenum t, GLintptr o, GLsizeiptr s, const void *d) { f_bsd++; fo_bsd(t, o, s, d); }
static VkResult fh_vkidle(VkQueue q) { f_vkidle++; return fo_vkidle(q); }
static void fh_vkdestroy(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { f_vkdestroy++; fo_vkdestroy(d, b, a); }

static long tramp_off(const char *name) {
    void *lib = dlopen("libGLESv2.so", RTLD_NOW);
    const uint32_t *ins = (const uint32_t *)dlsym(lib, name);
    for (int i = 1; ins && i < 10; i++)
        if (ins[i] == 0xd61f0200u && (ins[i - 1] & 0xFFC003FFu) == 0xF9400210u) return ((ins[i - 1] >> 10) & 0xFFF) * 8;
    return -1;
}

// GL: swap the entry in the CURRENT dispatch table (via TLS) — must have a current context.
static bool foreign_gl(const char *name, void *mine, void **orig) {
    long off = tramp_off(name);
    void **tls = (void **)__builtin_thread_pointer();
    void **table = (void **)tls[4];
    if (off < 0 || !table) return false;
    *orig = table[off / 8];
    table[off / 8] = mine;
    return true;
}

struct DrvFind { const char *name; uintptr_t bias; char path[256]; uintptr_t rw[8][2]; int nrw; };
static int drv_cb(dl_phdr_info *i, size_t, void *d) {
    DrvFind *f = (DrvFind *)d;
    if (!i->dlpi_name || !strstr(i->dlpi_name, f->name)) return 0;
    f->bias = i->dlpi_addr;
    snprintf(f->path, sizeof(f->path), "%s", i->dlpi_name);
    for (int k = 0; k < i->dlpi_phnum && f->nrw < 8; k++)
        if (i->dlpi_phdr[k].p_type == PT_LOAD && (i->dlpi_phdr[k].p_flags & PF_W)) {
            f->rw[f->nrw][0] = f->bias + i->dlpi_phdr[k].p_vaddr;
            f->rw[f->nrw][1] = f->rw[f->nrw][0] + i->dlpi_phdr[k].p_memsz;
            f->nrw++;
        }
    return 1;
}
static uintptr_t drv_sym(DrvFind &f, const char *sym) {
    int fd = open(f.path, O_RDONLY);
    struct stat st; fstat(fd, &st);
    uint8_t *m = (uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    auto *eh = (Elf64_Ehdr *)m;
    auto *sh = (Elf64_Shdr *)(m + eh->e_shoff);
    uintptr_t r = 0;
    for (int i = 0; i < eh->e_shnum && !r; i++) {
        if (sh[i].sh_type != SHT_DYNSYM) continue;
        auto *sy = (Elf64_Sym *)(m + sh[i].sh_offset);
        const char *str = (const char *)(m + sh[sh[i].sh_link].sh_offset);
        for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf64_Sym); k++)
            if (!strcmp(str + sy[k].st_name, sym)) { r = f.bias + sy[k].st_value; break; }
    }
    munmap(m, st.st_size);
    return r;
}
// Vulkan: classic value scan of the driver's writable/RELRO data for the function pointer.
static bool foreign_vk(const char *sym, void *mine, void **orig) {
    DrvFind f{"vulkan.adreno.so"};
    dl_iterate_phdr(drv_cb, &f);
    uintptr_t fn = f.bias ? drv_sym(f, sym) : 0;
    if (!fn) return false;
    long pg = sysconf(_SC_PAGESIZE);
    for (int s = 0; s < f.nrw; s++)
        for (uintptr_t a = f.rw[s][0]; a + 8 <= f.rw[s][1]; a += 8)
            if (*(uintptr_t *)a == fn) {
                mprotect((void *)(a & ~(pg - 1)), pg, PROT_READ | PROT_WRITE);
                *orig = (void *)fn;
                *(void **)a = mine;
                return true;
            }
    return false;  // not found (e.g. already replaced by someone else)
}

static int foreign_install() {
    int ok = 0;
    ok += foreign_gl("glFinish", (void *)fh_finish, (void **)&fo_finish);
    ok += foreign_gl("glBufferSubData", (void *)fh_bsd, (void **)&fo_bsd);
    ok += foreign_vk("_ZN11qglinternal15vkQueueWaitIdleEP9VkQueue_T", (void *)fh_vkidle, (void **)&fo_vkidle);
    ok += foreign_vk("_ZN11qglinternal15vkDestroyBufferEP10VkDevice_TP10VkBuffer_TPK21VkAllocationCallbacks", (void *)fh_vkdestroy, (void **)&fo_vkdestroy);
    return ok;
}

// A thunk in anonymous executable memory: calls x7(x0..x6). Code here counts as "another module".
typedef void (*Thunk)(...);
static Thunk make_thunk() {
    static const uint32_t code[] = {0xa9bf7bfd, 0x910003fd, 0xd63f00e0, 0xa8c17bfd, 0xd65f03c0};  // stp; mov x29,sp; blr x7; ldp; ret
    void *p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(p, code, sizeof(code));
    __builtin___clear_cache((char *)p, (char *)p + sizeof(code));
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC)) return nullptr;
    return (Thunk)p;
}

static const char *(*lab_wait)(int);
static const char *(*lab_mode)();

static int coexist_test(const char *conf, const char *dir) {
    const char *order = getenv("LAB_ORDER") ? getenv("LAB_ORDER") : "first";
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, nullptr, nullptr);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, sa);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    eglMakeCurrent(d, s, s, x);
    uint32_t ne = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr);  // load the Vulkan driver
    int fok = 0;
    if (!strcmp(order, "first")) {
        fok = foreign_install();
        printf("foreign hooks installed first: %d/4\n", fok);
        printf("hooks(pending): %s\n", lab_install(conf, dir, "lab.coexist"));
        printf("hooks: %s\n", lab_wait(10000));
    } else {  // late: LoadShift waits, the other module arrives 400 ms later
        printf("hooks(pending): %s\n", lab_install(conf, dir, "lab.coexist"));
        usleep(400000);
        fok = foreign_install();
        printf("foreign hooks installed after start: %d/4\n", fok);
        printf("hooks: %s\n", lab_wait(10000));
    }
    printf("mode: %s\n", lab_mode());

    // GL from the app (trusted): both modules must see their calls.
    GLuint b; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, 4 << 20, nullptr, GL_DYNAMIC_DRAW);
    static uint8_t data[2 << 20];
    for (int i = 0; i < 10; i++) glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(data), data);
    for (int i = 0; i < 20; i++) glFinish();
    printf("GL  foreign saw: glFinish %d/20, glBufferSubData %d/10\n", f_finish.load(), f_bsd.load());

    // Readback from "another module's" code must stay exact (not one frame late).
    GLuint rt, fbo;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    Thunk th = make_thunk();
    int exactForeign = 0, exactApp = 0;
    for (int f = 1; f <= 6; f++) {
        glClearColor(f / 10.0f, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        uint8_t px[4] = {};
        if (th) th(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px, (void *)glReadPixels);
        if (px[0] == (uint8_t)(f / 10.0f * 255 + 0.5f)) exactForeign++;
        glClearColor(f / 10.0f, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        uint8_t q[4] = {};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
        if (q[0] == (uint8_t)(f / 10.0f * 255 + 0.5f)) exactApp++;
    }
    printf("readback exact: from other module %d/6 (expect 6), from app %d/6 (async = fewer)\n", exactForeign, exactApp);

    // Vulkan: WaitIdle and destroys must still reach the other module.
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    VkInstance inst; vkCreateInstance(&ici, nullptr, &inst);
    uint32_t pc = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &pc, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    VkDevice dev; vkCreateDevice(pd, &dci, nullptr, &dev);
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    for (int i = 0; i < 5; i++) {
        vkQueueWaitIdle(q);
        VkBuffer tmp; vkCreateBuffer(dev, &bi, nullptr, &tmp); vkDestroyBuffer(dev, tmp, nullptr);
    }
    usleep(300000);  // let any deferred destroys run
    printf("VK  foreign saw: vkQueueWaitIdle %d/5, vkDestroyBuffer %d/5\n", f_vkidle.load(), f_vkdestroy.load());
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    lab_flush();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
    bool ok = f_finish == 20 && f_bsd == 10 && exactForeign == 6 && f_vkidle == 5 && f_vkdestroy == 5;
    printf("COEXIST %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 5) { puts("usage: lab <gles|vk> <lib> <conf> <dir>"); return 2; }
    void *h = dlopen(argv[2], RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    lab_install = (decltype(lab_install))dlsym(h, "loadshift_lab_install");
    lab_flush = (decltype(lab_flush))dlsym(h, "loadshift_lab_flush");
    lab_wait = (decltype(lab_wait))dlsym(h, "loadshift_lab_wait");
    lab_mode = (decltype(lab_mode))dlsym(h, "loadshift_lab_mode");
    const char *conf = slurp(argv[3]);
    if (!strcmp(argv[1], "gles")) return gles(conf, argv[4]);
    if (!strcmp(argv[1], "glbuf")) return glbuf_test(conf, argv[4]);
    if (!strcmp(argv[1], "probe")) return probe_test(conf, argv[4]);
    if (!strcmp(argv[1], "probe2")) return probe2_test(conf, argv[4]);
    if (!strcmp(argv[1], "coexist")) return coexist_test(conf, argv[4]);
    if (!strcmp(argv[1], "occlgl")) return occl_gl_test(conf, argv[4]);
    if (!strcmp(argv[1], "occlvk")) { printf("hooks: %s\n", lab_install(conf, argv[4], "lab.occlvk")); return occl_vk_test(); }
    if (!strcmp(argv[1], "vkidle")) { printf("hooks: %s\n", lab_install(conf, argv[4], "lab.vkidle")); return vkidle_test(); }
    printf("hooks: %s\n", lab_install(conf, argv[4], "lab.vk"));
    return vk();
}
