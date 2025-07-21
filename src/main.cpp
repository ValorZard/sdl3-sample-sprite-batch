#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_image/SDL_image.h>
#include <cmath>
#include <string_view>
#include <filesystem>
#include "common.h"

constexpr uint32_t windowStartWidth = 640;
constexpr uint32_t windowStartHeight = 480;

struct AppContext {
    SDL_Window* window;
    SDL_GPUDevice* device;
    //SDL_Renderer* renderer;
    //SDL_Texture* messageTex, *imageTex;
    SDL_FRect messageDest;
    SDL_AudioDeviceID audioDevice;
    Mix_Music* music;
    SDL_AppResult app_quit = SDL_APP_CONTINUE;
};

static SDL_GPUGraphicsPipeline* RenderPipeline;
static SDL_GPUSampler* Sampler;
static SDL_GPUTexture* Texture;
static SDL_GPUTransferBuffer* SpriteDataTransferBuffer;
static SDL_GPUBuffer* SpriteDataBuffer;

typedef struct SpriteInstance
{
    float x, y, z;
    float rotation;
    float w, h, padding_a, padding_b;
    float tex_u, tex_v, tex_w, tex_h;
    float r, g, b, a;
} SpriteInstance;

static const Uint32 SPRITE_COUNT = 8192;


SDL_AppResult SDL_Fail(){
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Error %s", SDL_GetError());
    return SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    // init the library, here we make a window so we only need the Video capabilities.
    if (not SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)){
        return SDL_Fail();
    }
    
    // init TTF
    if (not TTF_Init()) {
        return SDL_Fail();
    }
    
    // create a window
   
    SDL_Window* window = SDL_CreateWindow("SDL Minimal Sample", windowStartWidth, windowStartHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (not window){
        return SDL_Fail();
    }
    
    // asset loading
#if __ANDROID__
    std::filesystem::path basePath = "";   // on Android we do not want to use basepath. Instead, assets are available at the root directory.
#else
    auto basePathPtr = SDL_GetBasePath();
    if (not basePathPtr) {
        return SDL_Fail();
    }
    const std::filesystem::path basePath = basePathPtr;
#endif

    // SDL_GPU stuff
    auto device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
        true,
        NULL);

    if (device == NULL)
    {
        SDL_Log("GPUCreateDevice failed");
        return SDL_Fail();
    }


    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("GPUClaimWindow failed");
        return SDL_Fail();
    }

    SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;
    if (SDL_WindowSupportsGPUPresentMode(
        device,
        window,
        SDL_GPU_PRESENTMODE_IMMEDIATE
    )) {
        presentMode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    else if (SDL_WindowSupportsGPUPresentMode(
        device,
        window,
        SDL_GPU_PRESENTMODE_MAILBOX
    )) {
        presentMode = SDL_GPU_PRESENTMODE_MAILBOX;
    }

    SDL_SetGPUSwapchainParameters(
        device,
        window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        presentMode
    );

    SDL_srand(0);

    // Create the shaders
    SDL_GPUShader* vertShader = LoadShader(
        basePath.string().c_str(),
        device,
        "PullSpriteBatch.vert",
        0,
        1,
        1,
        0
    );

    SDL_GPUShader* fragShader = LoadShader(
		basePath.string().c_str(),
        device,
        "TexturedQuadColor.frag",
        1,
        0,
        0,
        0
    );

    SDL_GPUColorTargetDescription colorTargetDescriptions[1] = {SDL_GPUColorTargetDescription {
                .format = SDL_GetGPUSwapchainTextureFormat(device, window),
                .blend_state = {
                    .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                    .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .color_blend_op = SDL_GPU_BLENDOP_ADD,
                    .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                    .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                    .enable_blend = true,
                }
            } };

    auto graphicsPipelineTargetInfo = SDL_GPUGraphicsPipelineTargetInfo{
            .color_target_descriptions = colorTargetDescriptions,
            .num_color_targets = 1,
    };

    // Create the sprite render pipeline
    auto graphicsPipelineCreateInfo = SDL_GPUGraphicsPipelineCreateInfo{
        .vertex_shader = vertShader,
        .fragment_shader = fragShader,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info = graphicsPipelineTargetInfo,
    };
    RenderPipeline = SDL_CreateGPUGraphicsPipeline(
        device,
        &graphicsPipelineCreateInfo
    );

    SDL_ReleaseGPUShader(device, vertShader);
    SDL_ReleaseGPUShader(device, fragShader);

    // Load the image data
    SDL_Surface* imageData = LoadImage(basePath.string().c_str(), "ravioli_atlas.bmp", 4);
    if (imageData == NULL)
    {
        SDL_Log("Could not load image data!");
        return SDL_Fail();
    }

    auto transferBufferCreateInfo = SDL_GPUTransferBufferCreateInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size = (Uint32) imageData->w * imageData->h * 4
    };

    SDL_GPUTransferBuffer* textureTransferBuffer = SDL_CreateGPUTransferBuffer(
        device,
        &transferBufferCreateInfo
    );

    Uint8* textureTransferPtr = (Uint8*) SDL_MapGPUTransferBuffer(
        device,
        textureTransferBuffer,
        false
    );
    SDL_memcpy(textureTransferPtr, imageData->pixels, imageData->w * imageData->h * 4);
    SDL_UnmapGPUTransferBuffer(device, textureTransferBuffer);

    // Create the GPU resources
    auto textureCreateInfo = SDL_GPUTextureCreateInfo {
        .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = (Uint32) imageData->w,
            .height = (Uint32) imageData->h,
            .layer_count_or_depth = 1,
            .num_levels = 1,
    };

    Texture = SDL_CreateGPUTexture(
        device,
        &textureCreateInfo
    );

    auto samplerCreateInfo = SDL_GPUSamplerCreateInfo{
        .min_filter = SDL_GPU_FILTER_NEAREST,
            .mag_filter = SDL_GPU_FILTER_NEAREST,
            .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
    };

    Sampler = SDL_CreateGPUSampler(
        device,
        &samplerCreateInfo
    );

    auto transferBufferCreateInfo2 = SDL_GPUTransferBufferCreateInfo{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size = SPRITE_COUNT * sizeof(SpriteInstance)
    };

    SpriteDataTransferBuffer = SDL_CreateGPUTransferBuffer(
        device,
        &transferBufferCreateInfo2
    );

    auto bufferCreateInfo = SDL_GPUBufferCreateInfo{
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
            .size = SPRITE_COUNT * sizeof(SpriteInstance)
    };

    SpriteDataBuffer = SDL_CreateGPUBuffer(
        device,
        &bufferCreateInfo
    );

    // Transfer the up-front data
    SDL_GPUCommandBuffer* uploadCmdBuf = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmdBuf);

    auto textureTransferInfo = SDL_GPUTextureTransferInfo {
        .transfer_buffer = textureTransferBuffer,
            .offset = 0, /* Zeroes out the rest */
    };

    auto textureRegion = SDL_GPUTextureRegion {
        .texture = Texture,
            .w = (Uint32) imageData->w,
            .h = (Uint32) imageData->h,
            .d = 1
    };

    SDL_UploadToGPUTexture(
        copyPass,
        &textureTransferInfo,
        &textureRegion,
        false
    );

    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(uploadCmdBuf);

    SDL_DestroySurface(imageData);
    SDL_ReleaseGPUTransferBuffer(device, textureTransferBuffer);
    
    // load the font

    const auto fontPath = basePath / "Inter-VariableFont.ttf";
    TTF_Font* font = TTF_OpenFont(fontPath.string().c_str(), 36);
    if (not font) {
        return SDL_Fail();
    }

    // render the font to a surface
    const std::string_view text = "Hello SDL!";
    SDL_Surface* surfaceMessage = TTF_RenderText_Solid(font, text.data(), text.length(), { 255,255,255 });

    // make a texture from the surface
    //SDL_Texture* messageTex = SDL_CreateTextureFromSurface(renderer, surfaceMessage);

    // we no longer need the font or the surface, so we can destroy those now.
    TTF_CloseFont(font);
    SDL_DestroySurface(surfaceMessage);

    // load the SVG
    auto svg_surface = IMG_Load((basePath / "gs_tiger.svg").string().c_str());
    //SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, svg_surface);
    //SDL_DestroySurface(svg_surface);
    

    // get the on-screen dimensions of the text. this is necessary for rendering it
    /*
    auto messageTexProps = SDL_GetTextureProperties(messageTex);
    SDL_FRect text_rect{
            .x = 0,
            .y = 0,
            .w = float(SDL_GetNumberProperty(messageTexProps, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0)),
            .h = float(SDL_GetNumberProperty(messageTexProps, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0))
    };
    */

    // init SDL Mixer
    auto audioDevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (not audioDevice) {
        return SDL_Fail();
    }
    if (not Mix_OpenAudio(audioDevice, NULL)) {
        return SDL_Fail();
    }

    // load the music
    auto musicPath = basePath / "the_entertainer.ogg";
    auto music = Mix_LoadMUS(musicPath.string().c_str());
    if (not music) {
        return SDL_Fail();
    }

    // play the music (does not loop)
    Mix_PlayMusic(music, 0);
    
    // print some information about the window
    SDL_ShowWindow(window);
    {
        int width, height, bbwidth, bbheight;
        SDL_GetWindowSize(window, &width, &height);
        SDL_GetWindowSizeInPixels(window, &bbwidth, &bbheight);
        SDL_Log("Window size: %ix%i", width, height);
        SDL_Log("Backbuffer size: %ix%i", bbwidth, bbheight);
        if (width != bbwidth){
            SDL_Log("This is a highdpi environment.");
        }
    }

    // set up the application data
    *appstate = new AppContext{
       .window = window,
       .device = device,
       //.renderer = renderer,
       //.messageTex = messageTex,
       //.imageTex = NULL,
       //.messageDest = text_rect,
       .audioDevice = audioDevice,
       .music = music,
    };
    
    //SDL_SetRenderVSync(renderer, -1);   // enable vysnc
    
    SDL_Log("Application started successfully!");

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event* event) {
    auto* app = (AppContext*)appstate;
    
    if (event->type == SDL_EVENT_QUIT) {
        app->app_quit = SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

static float uCoords[4] = { 0.0f, 0.5f, 0.0f, 0.5f };
static float vCoords[4] = { 0.0f, 0.0f, 0.5f, 0.5f };
SDL_AppResult SDL_AppIterate(void *appstate) {
    auto* app = (AppContext*)appstate;

    // draw a color
    auto time = SDL_GetTicks() / 1000.f;
    auto red = (std::sin(time) + 1) / 2.0 * 255;
    auto green = (std::sin(time / 2) + 1) / 2.0 * 255;
    auto blue = (std::sin(time) * 2 + 1) / 2.0 * 255;
    
    Matrix4x4 cameraMatrix = Matrix4x4_CreateOrthographicOffCenter(
        0,
        640,
        480,
        0,
        0,
        -1
    );

    SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(app->device);
    if (cmdBuf == NULL)
    {
        SDL_Log("AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return SDL_Fail();
    }

    SDL_GPUTexture* swapchainTexture;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdBuf, app->window, &swapchainTexture, NULL, NULL)) {
        SDL_Log("WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        return SDL_Fail();
    }

    if (swapchainTexture != NULL)
    {
        // Build sprite instance transfer
        SpriteInstance* dataPtr = (SpriteInstance*) SDL_MapGPUTransferBuffer(
            app->device,
            SpriteDataTransferBuffer,
            true
        );

        for (Uint32 i = 0; i < SPRITE_COUNT; i += 1)
        {
            Sint32 ravioli = SDL_rand(4);
            dataPtr[i].x = (float)(SDL_rand(640));
            dataPtr[i].y = (float)(SDL_rand(480));
            dataPtr[i].z = 0;
            dataPtr[i].rotation = SDL_randf() * SDL_PI_F * 2;
            dataPtr[i].w = 32;
            dataPtr[i].h = 32;
            dataPtr[i].tex_u = uCoords[ravioli];
            dataPtr[i].tex_v = vCoords[ravioli];
            dataPtr[i].tex_w = 0.5f;
            dataPtr[i].tex_h = 0.5f;
            dataPtr[i].r = 1.0f;
            dataPtr[i].g = 1.0f;
            dataPtr[i].b = 1.0f;
            dataPtr[i].a = 1.0f;
        }

        SDL_UnmapGPUTransferBuffer(app->device, SpriteDataTransferBuffer);

        // Upload instance data
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);
        auto transferBufferLocation = SDL_GPUTransferBufferLocation {
            .transfer_buffer = SpriteDataTransferBuffer,
                .offset = 0
        };
        auto gpuBufferRegion = SDL_GPUBufferRegion{
            .buffer = SpriteDataBuffer,
                .offset = 0,
                .size = SPRITE_COUNT * sizeof(SpriteInstance)
        };

        SDL_UploadToGPUBuffer(
            copyPass,
            &transferBufferLocation,
            &gpuBufferRegion,
            true
        );
        SDL_EndGPUCopyPass(copyPass);

        // Render sprites
        auto colorTargetInfo = SDL_GPUColorTargetInfo {
            .texture = swapchainTexture,
            .clear_color = { 0, 0, 0, 1 },
            .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
                .cycle = false,
        };

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(
            cmdBuf,
            &colorTargetInfo,
            1,
            NULL
        );

        SDL_BindGPUGraphicsPipeline(renderPass, RenderPipeline);
        SDL_BindGPUVertexStorageBuffers(
            renderPass,
            0,
            &SpriteDataBuffer,
            1
        );
        auto textureSamplerBinding = SDL_GPUTextureSamplerBinding {
            .texture = Texture,
                .sampler = Sampler
        };

        SDL_BindGPUFragmentSamplers(
            renderPass,
            0,
            &textureSamplerBinding,
            1
        );
        SDL_PushGPUVertexUniformData(
            cmdBuf,
            0,
            &cameraMatrix,
            sizeof(Matrix4x4)
        );
        SDL_DrawGPUPrimitives(
            renderPass,
            SPRITE_COUNT * 6,
            1,
            0,
            0
        );

        SDL_EndGPURenderPass(renderPass);
    }

    SDL_SubmitGPUCommandBuffer(cmdBuf);

    return app->app_quit;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    auto* app = (AppContext*)appstate;
    if (app) {
        //SDL_DestroyRenderer(app->renderer);
        SDL_ReleaseWindowFromGPUDevice(app->device, app->window);
        SDL_DestroyWindow(app->window);
        SDL_DestroyGPUDevice(app->device);

        Mix_FadeOutMusic(1000);  // prevent the music from abruptly ending.
        Mix_FreeMusic(app->music); // this call blocks until the music has finished fading
        Mix_CloseAudio();
        SDL_CloseAudioDevice(app->audioDevice);

        delete app;
    }
    TTF_Quit();
    Mix_Quit();

    SDL_Log("Application quit successfully!");
    SDL_Quit();
}
