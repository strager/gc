#include "imgui.h"
#include <string.h>
#include <vector>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include "gc.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <algorithm>
#include <deque>

static int memfd;
// Pointer to the GC_State struct inside the debuggee.
static GC_State *state_ptr;

// Read memory from the debuggee process.
//
// args are ordered like memcpy.
//
// Returns false on error.
static bool GC_read(void *out, const void *ptr, size_t size) {
  //int rc = pread(memfd, out, size, (unsigned long)(ptr));
  int rc;
  rc = lseek(memfd, (unsigned long)(ptr), SEEK_SET);
  if (rc == -1) {
    fprintf(stderr, "error: failed to read memory: lseek: %s\n", strerror(errno));
    return false;
  }
  rc = read(memfd, out, size);
  if (rc == -1) {
    fprintf(stderr, "error: failed to read memory: read: %s\n", strerror(errno));
    return false;
  }
  if ((size_t)(rc) != size) {
    fprintf(stderr, "error: failed to read memory: got %d bytes, wanted %zu\n", rc, size);
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) {
    printf("usage: %s <memfd(hex)> <state_ptr(hex)> [<pid>]\n", argv[0]);
    return 1;
  }

  unsigned long memfd_raw;
  if (sscanf(argv[1], "%lx", &memfd_raw) != 1) {
    printf("error parsing memfd arg\n");
    return 1;
  }
  memfd = (int)(memfd_raw);

  unsigned long state_ptr_raw;
  if (sscanf(argv[2], "%lx", &state_ptr_raw) != 1) {
    printf("error parsing state_ptr arg\n");
    return 1;
  }
  state_ptr = (GC_State*)(state_ptr_raw);

  unsigned long kill_pid_raw;
  if (sscanf(argv[2], "%lx", &kill_pid_raw) != 1) {
    printf("error parsing state_ptr arg\n");
    return 1;
  }
  pid_t kill_pid = kill_pid_raw;

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    printf("Error: SDL_Init(): %s\n", SDL_GetError());
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  SDL_Window* window = SDL_CreateWindow("gcviz", (int)(1280 * main_scale), (int)(800 * main_scale), window_flags);
  if (window == nullptr) {
    printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
    return 1;
  }
  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (gl_context == nullptr) {
    printf("Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
    return 1;
  }

  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO(); (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  style.FontSizeBase = 20.0f;
  style.FrameRounding = 3.0f;
  style.GrabRounding = 3.0f;

  style.ScaleAllSizes(main_scale);
  style.FontScaleDpi = main_scale;

  ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init(nullptr);

  io.Fonts->AddFontFromFileTTF("/home/strager/Comic Code Coding Essentials/Tabular Type Foundry - Comic Code Medium.otf");

  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  bool done = false;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        done = true;
      }
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
        done = true;
      }
    }

    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(10);
      continue;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    {
      ImGui::Begin("Hello, world!");
      ImDrawList* draw_list = ImGui::GetWindowDrawList();

      ImU32 bg_color = ImGui::GetColorU32(IM_COL32(0, 0, 0, 255));
      ImU32 used_color = ImGui::GetColorU32(IM_COL32(200, 0, 0, 255));

      GC_State gc;
      if (GC_read(&gc, state_ptr, sizeof(gc))) {
        struct ChunkMeta {
          GC_Chunk *c_ptr;
          GC_Chunk header;
        };
        std::deque<ChunkMeta> chunk_metas;
        GC_Chunk *c_ptr = gc.chunks;
        while (c_ptr != nullptr) {
          GC_Chunk c;
          if (GC_read(&c, c_ptr, sizeof(c))) {
            chunk_metas.push_back(ChunkMeta{
                .c_ptr = c_ptr,
                .header = c,
              });
            c_ptr = c.next;
          } else {
            break;
          }
        }
        std::stable_sort(chunk_metas.begin(), chunk_metas.end(), [](const ChunkMeta &x, const ChunkMeta &y) -> int {
          return x.header.size_class < y.header.size_class;
        });

        ImGui::Text("alloc_count = %lu", gc.alloc_count);
        ImGui::Text("gc_count = %lu", gc.gc_count);
        ImGui::Text("chunks.count = %zu", chunk_metas.size());

        std::vector<uint8_t> tags;
        for (ChunkMeta &c : chunk_metas) {
          ImVec2 p = ImGui::GetCursorScreenPos();
          p = ImVec2{floor(p.x), floor(p.y)};
          ImVec2 object_size = {2, 20};
          tags.resize((c.header.object_count+7)/8);  // Assumes 1 bit per tag.
          if (GC_read(tags.data(), &c.c_ptr->tags[0], tags.size() * sizeof(uint8_t))) {
            draw_list->AddRectFilled({ p.x, p.y }, { p.x+object_size.x*c.header.object_count, p.y+object_size.y }, bg_color, 0.0f, ImDrawFlags_None);
            for (size_t i = 0; i < c.header.object_count; i += 1) {
              switch (GC_chunktags_get_object_tag(tags.data(), i)) {
              case GC_ObjectTag_Free:
                break;
              case GC_ObjectTag_Used:
                draw_list->AddRectFilled({ p.x+object_size.x*i, p.y }, { p.x+object_size.x*(i+1), p.y+object_size.y }, used_color, 0.0f, ImDrawFlags_None);
                break;
              }
            }
          }
          ImGui::Text("chunk %zu", c.header.size_class);
        }
      }
      ImGui::End();
    }

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DestroyContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  pid_t parent_pid = getppid();
  if (parent_pid == kill_pid && parent_pid != 0 && parent_pid != -1) {
    kill(parent_pid, SIGINT);
  }

  return 0;
}
