/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/emulator_window.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/system.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/virtual_key.h"

// Autogenerated by `xb premake`.
#include "build/version.h"

DECLARE_bool(debug);

DEFINE_bool(fullscreen, false, "Whether to launch the emulator in fullscreen.",
            "Display");

DEFINE_string(
    postprocess_antialiasing, "",
    "Post-processing anti-aliasing effect to apply to the image output of the "
    "game.\n"
    "Using post-process anti-aliasing is heavily recommended when AMD "
    "FidelityFX Contrast Adaptive Sharpening or Super Resolution 1.0 is "
    "active.\n"
    "Use: [none, fxaa, fxaa_extreme]\n"
    " none (or any value not listed here):\n"
    "  Don't alter the original image.\n"
    " fxaa:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, normal quality preset (12)."
    "\n"
    " fxaa_extreme:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, extreme quality preset "
    "(39).",
    "Display");
DEFINE_string(
    postprocess_scaling_and_sharpening, "",
    "Post-processing effect to use for resampling and/or sharpening of the "
    "final display output.\n"
    "Use: [bilinear, cas, fsr]\n"
    " bilinear (or any value not listed here):\n"
    "  Original image at 1:1, simple bilinear stretching for resampling.\n"
    " cas:\n"
    "  Use AMD FidelityFX Contrast Adaptive Sharpening (CAS) for sharpening "
    "at scaling factors of up to 2x2, with additional bilinear stretching for "
    "larger factors.\n"
    " fsr:\n"
    "  Use AMD FidelityFX Super Resolution 1.0 (FSR) for highest-quality "
    "upscaling, or AMD FidelityFX Contrast Adaptive Sharpening for sharpening "
    "while not scaling or downsampling.\n"
    "  For scaling by factors of more than 2x2, multiple FSR passes are done.",
    "Display");
DEFINE_double(
    postprocess_ffx_cas_additional_sharpness,
    xe::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
    "Additional sharpness for AMD FidelityFX Contrast Adaptive Sharpening "
    "(CAS), from 0 to 1.\n"
    "Higher is sharper.",
    "Display");
DEFINE_uint32(
    postprocess_ffx_fsr_max_upsampling_passes,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
    "Maximum number of upsampling passes performed in AMD FidelityFX Super "
    "Resolution 1.0 (FSR) before falling back to bilinear stretching after the "
    "final pass.\n"
    "Each pass upscales only to up to 2x2 the previous size. If the game "
    "outputs a 1280x720 image, 1 pass will upscale it to up to 2560x1440 "
    "(below 4K), after 2 passes it will be upscaled to a maximum of 5120x2880 "
    "(including 3840x2160 for 4K), and so on.\n"
    "This variable has no effect if the display resolution isn't very high, "
    "but may be reduced on resolutions like 4K or 8K in case the performance "
    "impact of multiple FSR upsampling passes is too high, or if softer edges "
    "are desired.\n"
    "The default value is the maximum internally supported by Xenia.",
    "Display");
DEFINE_double(
    postprocess_ffx_fsr_sharpness_reduction,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
    "Sharpness reduction for AMD FidelityFX Super Resolution 1.0 (FSR), in "
    "stops.\n"
    "Lower is sharper.",
    "Display");
// Dithering to 8bpc is enabled by default since the effect is minor, only
// effects what can't be shown normally by host displays, and nothing is changed
// by it for 8bpc source without resampling.
DEFINE_bool(
    postprocess_dither, true,
    "Dither the final image output from the internal precision to 8 bits per "
    "channel so gradients are smoother.\n"
    "On a 10bpc display, the lower 2 bits will still be kept, but noise will "
    "be added to them - disabling may be recommended for 10bpc, but it "
    "depends on the 10bpc displaying capabilities of the actual display used.",
    "Display");

namespace xe {
namespace app {

using xe::ui::FileDropEvent;
using xe::ui::KeyEvent;
using xe::ui::MenuItem;
using xe::ui::UIEvent;

const std::string kBaseTitle = "Xenia-canary";

EmulatorWindow::EmulatorWindow(Emulator* emulator,
                               ui::WindowedAppContext& app_context)
    : emulator_(emulator),
      app_context_(app_context),
      window_listener_(*this),
      window_(ui::Window::Create(app_context, kBaseTitle, 1280, 720)),
      imgui_drawer_(
          std::make_unique<ui::ImGuiDrawer>(window_.get(), kZOrderImGui)),
      display_config_game_config_load_callback_(
          new DisplayConfigGameConfigLoadCallback(*emulator, *this)) {
  base_title_ = kBaseTitle +
#ifdef DEBUG
#if _NO_DEBUG_HEAP == 1
                " DEBUG"
#else
                " CHECKED"
#endif
#endif
                " ("
#ifdef XE_BUILD_IS_PR
                "PR#" XE_BUILD_PR_NUMBER " " XE_BUILD_PR_REPO
                " " XE_BUILD_PR_BRANCH "@" XE_BUILD_PR_COMMIT_SHORT " against "
#endif
                XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE
                ")";
}

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(
    Emulator* emulator, ui::WindowedAppContext& app_context) {
  assert_true(app_context.IsInUIThread());
  std::unique_ptr<EmulatorWindow> emulator_window(
      new EmulatorWindow(emulator, app_context));
  if (!emulator_window->Initialize()) {
    return nullptr;
  }
  return emulator_window;
}

EmulatorWindow::~EmulatorWindow() {
  // Notify the ImGui drawer that the immediate drawer is being destroyed.
  ShutdownGraphicsSystemPresenterPainting();
}

ui::Presenter* EmulatorWindow::GetGraphicsSystemPresenter() const {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  return graphics_system ? graphics_system->presenter() : nullptr;
}

void EmulatorWindow::SetupGraphicsSystemPresenterPainting() {
  ShutdownGraphicsSystemPresenterPainting();

  if (!window_) {
    return;
  }

  ui::Presenter* presenter = GetGraphicsSystemPresenter();
  if (!presenter) {
    return;
  }

  ApplyDisplayConfigForCvars();

  window_->SetPresenter(presenter);

  immediate_drawer_ =
      emulator_->graphics_system()->provider()->CreateImmediateDrawer();
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(presenter);
    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter,
                                                  immediate_drawer_.get());
    Profiler::SetUserIO(kZOrderProfiler, window_.get(), presenter,
                        immediate_drawer_.get());
  }
}

void EmulatorWindow::ShutdownGraphicsSystemPresenterPainting() {
  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);
  imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
  immediate_drawer_.reset();
  if (window_) {
    window_->SetPresenter(nullptr);
  }
}

void EmulatorWindow::OnEmulatorInitialized() {
  emulator_initialized_ = true;
  window_->SetMainMenuEnabled(true);
  // When the user can see that the emulator isn't initializing anymore (the
  // menu isn't disabled), enter fullscreen if requested.
  if (cvars::fullscreen) {
    SetFullscreen(true);
  }
}

void EmulatorWindow::EmulatorWindowListener::OnClosing(ui::UIEvent& e) {
  emulator_window_.app_context_.QuitFromUIThread();
}

void EmulatorWindow::EmulatorWindowListener::OnFileDrop(ui::FileDropEvent& e) {
  emulator_window_.FileDrop(e.filename());
}

void EmulatorWindow::EmulatorWindowListener::OnKeyDown(ui::KeyEvent& e) {
  emulator_window_.OnKeyDown(e);
}

void EmulatorWindow::DisplayConfigGameConfigLoadCallback::PostGameConfigLoad() {
  emulator_window_.ApplyDisplayConfigForCvars();
}

void EmulatorWindow::DisplayConfigDialog::OnDraw(ImGuiIO& io) {
  gpu::GraphicsSystem* graphics_system =
      emulator_window_.emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  // In the top-left corner so it's close to the menu bar from where it was
  // opened.
  // Origin Y coordinate 20 was taken from the Dear ImGui demo.
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  // Alpha from Dear ImGui tooltips (0.35 from the overlay provides too low
  // visibility). Translucent so some effect of the changes can still be seen
  // through it.
  ImGui::SetNextWindowBgAlpha(0.6f);
  bool dialog_open = true;
  if (!ImGui::Begin("Post-processing", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    return;
  }
  // Even if the close button has been pressed, still paint everything not to
  // have one frame with an empty window.

  // Prevent user confusion which has been reported multiple times.
  ImGui::TextUnformatted("All effects can be used on GPUs of any brand.");
  ImGui::Spacing();

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    if (ImGui::TreeNodeEx(
            "Anti-aliasing",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      gpu::CommandProcessor::SwapPostEffect current_swap_post_effect =
          command_processor->GetDesiredSwapPostEffect();
      int new_swap_post_effect_index = int(current_swap_post_effect);
      ImGui::RadioButton("None", &new_swap_post_effect_index,
                         int(gpu::CommandProcessor::SwapPostEffect::kNone));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing 3.11 (FXAA), normal quality",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaa));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing 3.11 (FXAA), extreme quality",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme));
      gpu::CommandProcessor::SwapPostEffect new_swap_post_effect =
          gpu::CommandProcessor::SwapPostEffect(new_swap_post_effect_index);
      if (current_swap_post_effect != new_swap_post_effect) {
        command_processor->SetDesiredSwapPostEffect(new_swap_post_effect);
      }

      // Override the values in the cvars to save them to the config at exit if
      // the user has set them to anything new.
      if (GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing) !=
          new_swap_post_effect) {
        OVERRIDE_string(postprocess_antialiasing,
                        GetCvarValueForSwapPostEffect(new_swap_post_effect));
      }

      ImGui::TreePop();
    }
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    const ui::Presenter::GuestOutputPaintConfig& current_presenter_config =
        presenter->GetGuestOutputPaintConfigFromUIThread();
    ui::Presenter::GuestOutputPaintConfig new_presenter_config =
        current_presenter_config;

    if (ImGui::TreeNodeEx(
            "Resampling and sharpening",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      // Filtering effect.
      int new_effect_index = int(new_presenter_config.GetEffect());
      ImGui::RadioButton(
          "None / bilinear", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear));
      ImGui::RadioButton(
          "AMD FidelityFX Contrast Adaptive Sharpening (CAS)",
          &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kCas));
      ImGui::RadioButton(
          "AMD FidelityFX Super Resolution 1.0 (FSR)", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kFsr));
      new_presenter_config.SetEffect(
          ui::Presenter::GuestOutputPaintConfig::Effect(new_effect_index));

      // effect_description must be one complete, but short enough, sentence per
      // line, as TextWrapped doesn't work correctly in auto-resizing windows
      // (in the initial frames, the window becomes extremely tall, and widgets
      // added after the wrapped text have no effect on the width of the text).
      const char* effect_description = nullptr;
      switch (new_presenter_config.GetEffect()) {
        case ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear:
          effect_description =
              "Simple bilinear filtering is done if resampling is needed.\n"
              "Otherwise, only anti-aliasing is done if enabled, or displaying "
              "as is.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
          effect_description =
              "Sharpening and resampling to up to 2x2 to improve the fidelity "
              "of details.\n"
              "For scaling by more than 2x2, bilinear stretching is done "
              "afterwards.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
          effect_description =
              "High-quality edge-preserving upscaling to arbitrary target "
              "resolutions.\n"
              "For scaling by more than 2x2, multiple upsampling passes are "
              "done.\n"
              "If not upscaling, Contrast Adaptive Sharpening (CAS) is used "
              "instead.";
          break;
      }
      if (effect_description) {
        ImGui::TextUnformatted(effect_description);
      }

      if (new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kCas ||
          new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
        if (effect_description) {
          ImGui::Spacing();
        }

        ImGui::TextUnformatted(
            "FXAA is highly recommended when using CAS or FSR.");

        ImGui::Spacing();

        // 2 decimal places is more or less enough precision for the sharpness
        // given the minor visual effect of small changes, the width of the
        // slider, and readability convenience (2 decimal places is like an
        // integer percentage). However, because Dear ImGui parses the string
        // representation of the number and snaps the value to it internally,
        // 2 decimal places actually offer less precision than the slider itself
        // does. This is especially prominent in the low range of the non-linear
        // FSR sharpness reduction slider. 3 decimal places are optimal in this
        // case.

        if (new_presenter_config.GetEffect() ==
            ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
          float fsr_sharpness_reduction =
              new_presenter_config.GetFsrSharpnessReduction();
          ImGui::TextUnformatted(
              "FSR sharpness reduction when upscaling (lower is sharper):");
          // Power 2.0 as the reduction is in stops, used in exp2.
          ImGui::SliderFloat(
              "##FSRSharpnessReduction", &fsr_sharpness_reduction,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMin,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMax,
              "%.3f stops", 2.0f);
          ImGui::SameLine();
          if (ImGui::Button("Reset##ResetFSRSharpnessReduction")) {
            fsr_sharpness_reduction = ui::Presenter::GuestOutputPaintConfig ::
                kFsrSharpnessReductionDefault;
          }
          new_presenter_config.SetFsrSharpnessReduction(
              fsr_sharpness_reduction);
        }

        float cas_additional_sharpness =
            new_presenter_config.GetCasAdditionalSharpness();
        ImGui::TextUnformatted(
            new_presenter_config.GetEffect() ==
                    ui::Presenter::GuestOutputPaintConfig::Effect::kFsr
                ? "CAS additional sharpness when not upscaling (higher is "
                  "sharper):"
                : "CAS additional sharpness (higher is sharper):");
        ImGui::SliderFloat(
            "##CASAdditionalSharpness", &cas_additional_sharpness,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMin,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMax,
            "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##ResetCASAdditionalSharpness")) {
          cas_additional_sharpness = ui::Presenter::GuestOutputPaintConfig ::
              kCasAdditionalSharpnessDefault;
        }
        new_presenter_config.SetCasAdditionalSharpness(
            cas_additional_sharpness);

        // There's no need to expose the setting for the maximum number of FSR
        // EASU passes as it's largely meaningless if the user doesn't have a
        // very high-resolution monitor compared to the original image size as
        // most of the values of the slider will have no effect, and that's just
        // very fine-grained performance control for a fixed-overhead pass only
        // for huge screen resolutions.
      }

      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Dithering", ImGuiTreeNodeFlags_Framed |
                                           ImGuiTreeNodeFlags_DefaultOpen)) {
      bool dither = current_presenter_config.GetDither();
      ImGui::Checkbox(
          "Dither the final output to 8bpc to make gradients smoother",
          &dither);
      new_presenter_config.SetDither(dither);

      ImGui::TreePop();
    }

    presenter->SetGuestOutputPaintConfigFromUIThread(new_presenter_config);

    // Override the values in the cvars to save them to the config at exit if
    // the user has set them to anything new.
    ui::Presenter::GuestOutputPaintConfig cvars_presenter_config =
        GetGuestOutputPaintConfigForCvars();
    if (cvars_presenter_config.GetEffect() !=
        new_presenter_config.GetEffect()) {
      OVERRIDE_string(postprocess_scaling_and_sharpening,
                      GetCvarValueForGuestOutputPaintEffect(
                          new_presenter_config.GetEffect()));
    }
    if (cvars_presenter_config.GetCasAdditionalSharpness() !=
        new_presenter_config.GetCasAdditionalSharpness()) {
      OVERRIDE_double(postprocess_ffx_cas_additional_sharpness,
                      new_presenter_config.GetCasAdditionalSharpness());
    }
    if (cvars_presenter_config.GetFsrSharpnessReduction() !=
        new_presenter_config.GetFsrSharpnessReduction()) {
      OVERRIDE_double(postprocess_ffx_fsr_sharpness_reduction,
                      new_presenter_config.GetFsrSharpnessReduction());
    }
    if (cvars_presenter_config.GetDither() !=
        new_presenter_config.GetDither()) {
      OVERRIDE_bool(postprocess_dither, new_presenter_config.GetDither());
    }
  }

  ImGui::End();

  if (!dialog_open) {
    emulator_window_.ToggleDisplayConfigDialog();
    // `this` might have been destroyed by ToggleDisplayConfigDialog.
    return;
  }
}

bool EmulatorWindow::Initialize() {
  window_->AddListener(&window_listener_);
  window_->AddInputListener(&window_listener_, kZOrderEmulatorWindowInput);

  // Main menu.
  // FIXME: This code is really messy.
  auto main_menu = MenuItem::Create(MenuItem::Type::kNormal);
  auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, "&File");
  {
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Open...", "Ctrl+O",
                         std::bind(&EmulatorWindow::FileOpen, this)));
#ifdef DEBUG
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Close",
                         std::bind(&EmulatorWindow::FileClose, this)));
#endif  // #ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show content directory...",
        std::bind(&EmulatorWindow::ShowContentDirectory, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "E&xit", "Alt+F4",
                         [this]() { window_->RequestClose(); }));
  }
  main_menu->AddChild(std::move(file_menu));

  // CPU menu.
  auto cpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&CPU");
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Reset Time Scalar", "Numpad *",
        std::bind(&EmulatorWindow::CpuTimeScalarReset, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar /= 2", "Numpad -",
        std::bind(&EmulatorWindow::CpuTimeScalarSetHalf, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Time Scalar *= 2", "Numpad +",
        std::bind(&EmulatorWindow::CpuTimeScalarSetDouble, this)));
  }
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "Toggle Profiler &Display", "F3",
                                        []() { Profiler::ToggleDisplay(); }));
    cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "&Pause/Resume Profiler", "`",
                                        []() { Profiler::TogglePause(); }));
  }
  cpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break and Show Guest Debugger",
        "Pause/Break", std::bind(&EmulatorWindow::CpuBreakIntoDebugger, this)));
    cpu_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break into Host Debugger",
        "Ctrl+Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoHostDebugger, this)));
  }
  main_menu->AddChild(std::move(cpu_menu));

  // GPU menu.
  auto gpu_menu = MenuItem::Create(MenuItem::Type::kPopup, "&GPU");
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Trace Frame", "F4",
                         std::bind(&EmulatorWindow::GpuTraceFrame, this)));
  }
  gpu_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    gpu_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Clear Runtime Caches", "F5",
                         std::bind(&EmulatorWindow::GpuClearCaches, this)));
  }
  main_menu->AddChild(std::move(gpu_menu));

  // Display menu.
  auto display_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Display");
  {
    display_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Post-processing settings", "F6",
        std::bind(&EmulatorWindow::ToggleDisplayConfigDialog, this)));
  }
  display_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    display_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Fullscreen", "F11",
                         std::bind(&EmulatorWindow::ToggleFullscreen, this)));
  }
  main_menu->AddChild(std::move(display_menu));

  // HID menu.
  auto hid_menu = MenuItem::Create(MenuItem::Type::kPopup, "&HID");
  {
    hid_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Toggle controller vibration", "",
        std::bind(&EmulatorWindow::ToggleControllerVibration, this)));
  }
  main_menu->AddChild(std::move(hid_menu));

  // Help menu.
  auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Help");
  {
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "FA&Q...", "F1",
                         std::bind(&EmulatorWindow::ShowFAQ, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Game &compatibility...",
                         std::bind(&EmulatorWindow::ShowCompatibility, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Build commit on GitHub...", "F2",
        std::bind(&EmulatorWindow::ShowBuildCommit, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Recent changes on GitHub...", [this]() {
          LaunchWebBrowser(
              "https://github.com/xenia-project/xenia/compare/" XE_BUILD_COMMIT
              "..." XE_BUILD_BRANCH);
        }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&About...",
        [this]() { LaunchWebBrowser("https://xenia.jp/about/"); }));
  }
  main_menu->AddChild(std::move(help_menu));

  window_->SetMainMenu(std::move(main_menu));

  window_->SetMainMenuEnabled(false);

  UpdateTitle();

  if (!window_->Open()) {
    XELOGE("Failed to open the platform window");
    return false;
  }

  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);

  return true;
}

const char* EmulatorWindow::GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect) {
  switch (effect) {
    case gpu::CommandProcessor::SwapPostEffect::kFxaa:
      return "fxaa";
    case gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme:
      return "fxaa_extreme";
    default:
      return "";
  }
}

gpu::CommandProcessor::SwapPostEffect
EmulatorWindow::GetSwapPostEffectForCvarValue(const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaa)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return gpu::CommandProcessor::SwapPostEffect::kNone;
}

const char* EmulatorWindow::GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  switch (effect) {
    case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
      return "cas";
    case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
      return "fsr";
    default:
      return "";
  }
}

ui::Presenter::GuestOutputPaintConfig::Effect
EmulatorWindow::GetGuestOutputPaintEffectForCvarValue(
    const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kCas)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kCas;
  }
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kFsr)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kFsr;
  }
  return ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear;
}

ui::Presenter::GuestOutputPaintConfig
EmulatorWindow::GetGuestOutputPaintConfigForCvars() {
  ui::Presenter::GuestOutputPaintConfig paint_config;
  paint_config.SetAllowOverscanCutoff(true);
  paint_config.SetEffect(GetGuestOutputPaintEffectForCvarValue(
      cvars::postprocess_scaling_and_sharpening));
  paint_config.SetCasAdditionalSharpness(
      float(cvars::postprocess_ffx_cas_additional_sharpness));
  paint_config.SetFsrMaxUpsamplingPasses(
      cvars::postprocess_ffx_fsr_max_upsampling_passes);
  paint_config.SetFsrSharpnessReduction(
      float(cvars::postprocess_ffx_fsr_sharpness_reduction));
  paint_config.SetDither(cvars::postprocess_dither);
  return paint_config;
}

void EmulatorWindow::ApplyDisplayConfigForCvars() {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    command_processor->SetDesiredSwapPostEffect(
        GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing));
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    presenter->SetGuestOutputPaintConfigFromUIThread(
        GetGuestOutputPaintConfigForCvars());
  }
}

void EmulatorWindow::OnKeyDown(ui::KeyEvent& e) {
  if (!emulator_initialized_) {
    return;
  }

  switch (e.virtual_key()) {
    case ui::VirtualKey::kO: {
      if (!e.is_ctrl_pressed()) {
        return;
      }
      FileOpen();
    } break;
    case ui::VirtualKey::kMultiply: {
      CpuTimeScalarReset();
    } break;
    case ui::VirtualKey::kSubtract: {
      CpuTimeScalarSetHalf();
    } break;
    case ui::VirtualKey::kAdd: {
      CpuTimeScalarSetDouble();
    } break;

    case ui::VirtualKey::kF3: {
      Profiler::ToggleDisplay();
    } break;

    case ui::VirtualKey::kF4: {
      GpuTraceFrame();
    } break;
    case ui::VirtualKey::kF5: {
      GpuClearCaches();
    } break;

    case ui::VirtualKey::kF6: {
      ToggleDisplayConfigDialog();
    } break;
    case ui::VirtualKey::kF11: {
      ToggleFullscreen();
    } break;
    case ui::VirtualKey::kEscape: {
      // Allow users to escape fullscreen (but not enter it).
      if (!window_->IsFullscreen()) {
        return;
      }
      SetFullscreen(false);
    } break;

#ifdef DEBUG
    case ui::VirtualKey::kF7: {
      // Save to file
      // TODO: Choose path based on user input, or from options
      // TODO: Spawn a new thread to do this.
      emulator()->SaveToFile("test.sav");
    } break;
    case ui::VirtualKey::kF8: {
      // Restore from file
      // TODO: Choose path from user
      // TODO: Spawn a new thread to do this.
      emulator()->RestoreFromFile("test.sav");
    } break;
#endif  // #ifdef DEBUG

    case ui::VirtualKey::kPause: {
      CpuBreakIntoDebugger();
    } break;
    case ui::VirtualKey::kCancel: {
      CpuBreakIntoHostDebugger();
    } break;

    case ui::VirtualKey::kF1: {
      ShowFAQ();
    } break;

    case ui::VirtualKey::kF2: {
      ShowBuildCommit();
    } break;

    default:
      return;
  }

  e.set_handled(true);
}

void EmulatorWindow::FileDrop(const std::filesystem::path& filename) {
  if (!emulator_initialized_) {
    return;
  }
  auto result = emulator_->LaunchPath(filename);
  if (XFAILED(result)) {
    // TODO: Display a message box.
    XELOGE("Failed to launch target: {:08X}", result);
  }
}

void EmulatorWindow::FileOpen() {
  std::filesystem::path path;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Xbox Executable (*.xex)", "*.xex"},
      //{"Content Package (*.xcp)", "*.xcp" },
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_.get())) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }

  if (!path.empty()) {
    // Normalize the path and make absolute.
    auto abs_path = std::filesystem::absolute(path);
    auto result = emulator_->LaunchPath(abs_path);
    if (XFAILED(result)) {
      // TODO: Display a message box.
      XELOGE("Failed to launch target: {:08X}", result);
    }
  }
}

void EmulatorWindow::FileClose() {
  if (emulator_->is_title_open()) {
    emulator_->TerminateTitle();
  }
}

void EmulatorWindow::ShowContentDirectory() {
  std::filesystem::path target_path;

  auto content_root = emulator_->content_root();
  if (!emulator_->is_title_open() || !emulator_->kernel_state()) {
    target_path = content_root;
  } else {
    // TODO(gibbed): expose this via ContentManager?
    auto title_id =
        fmt::format("{:08X}", emulator_->kernel_state()->title_id());
    auto package_root = content_root / title_id;
    target_path = package_root;
  }

  if (!std::filesystem::exists(target_path)) {
    std::filesystem::create_directories(target_path);
  }

  LaunchFileExplorer(target_path);
}

void EmulatorWindow::CpuTimeScalarReset() {
  Clock::set_guest_time_scalar(1.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetHalf() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() / 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuTimeScalarSetDouble() {
  Clock::set_guest_time_scalar(Clock::guest_time_scalar() * 2.0);
  UpdateTitle();
}

void EmulatorWindow::CpuBreakIntoDebugger() {
  if (!cvars::debug) {
    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Xenia Debugger",
                                        "Xenia must be launched with the "
                                        "--debug flag in order to enable "
                                        "debugging.");
    return;
  }
  auto processor = emulator()->processor();
  if (processor->execution_state() == cpu::ExecutionState::kRunning) {
    // Currently running, so interrupt (and show the debugger).
    processor->Pause();
  } else {
    // Not running, so just bring the debugger into focus.
    processor->ShowDebugger();
  }
}

void EmulatorWindow::CpuBreakIntoHostDebugger() { xe::debugging::Break(); }

void EmulatorWindow::GpuTraceFrame() {
  emulator()->graphics_system()->RequestFrameTrace();
}

void EmulatorWindow::GpuClearCaches() {
  emulator()->graphics_system()->ClearCaches();
}

void EmulatorWindow::SetFullscreen(bool fullscreen) {
  if (window_->IsFullscreen() == fullscreen) {
    return;
  }
  window_->SetFullscreen(fullscreen);
  window_->SetCursorVisibility(fullscreen
                                   ? ui::Window::CursorVisibility::kAutoHidden
                                   : ui::Window::CursorVisibility::kVisible);
}

void EmulatorWindow::ToggleFullscreen() {
  SetFullscreen(!window_->IsFullscreen());
}

void EmulatorWindow::ToggleDisplayConfigDialog() {
  if (!display_config_dialog_) {
    display_config_dialog_ = std::unique_ptr<DisplayConfigDialog>(
        new DisplayConfigDialog(imgui_drawer_.get(), *this));
  } else {
    display_config_dialog_.reset();
  }
}

void EmulatorWindow::ToggleControllerVibration() {
  emulator()->input_system()->ToggleVibration();
}

void EmulatorWindow::ShowCompatibility() {
  const std::string_view base_url =
      "https://github.com/xenia-project/game-compatibility/issues";
  std::string url;
  // Avoid searching for a title ID of "00000000".
  uint32_t title_id = emulator_->title_id();
  if (!title_id) {
    url = base_url;
  } else {
    url = fmt::format("{}?q=is%3Aissue+is%3Aopen+{:08X}", base_url, title_id);
  }
  LaunchWebBrowser(url);
}

void EmulatorWindow::ShowFAQ() {
  LaunchWebBrowser("https://github.com/xenia-project/xenia/wiki/FAQ");
}

void EmulatorWindow::ShowBuildCommit() {
#ifdef XE_BUILD_IS_PR
  LaunchWebBrowser(
      "https://github.com/xenia-project/xenia/pull/" XE_BUILD_PR_NUMBER);
#else
  LaunchWebBrowser(
      "https://github.com/xenia-project/xenia/commit/" XE_BUILD_COMMIT);
#endif
}

void EmulatorWindow::UpdateTitle() {
  xe::StringBuffer sb;
  sb.Append(base_title_);

  // Title information, if available
  if (emulator()->is_title_open()) {
    sb.AppendFormat(u8" | [{:08X}", emulator()->title_id());
    auto title_version = emulator()->title_version();
    if (!title_version.empty()) {
      sb.Append(u8" v");
      sb.Append(title_version);
    }
    sb.Append(u8"]");

    auto title_name = emulator()->title_name();
    if (!title_name.empty()) {
      sb.Append(u8" ");
      sb.Append(title_name);
    }
  }

  // Graphics system name, if available
  auto graphics_system = emulator()->graphics_system();
  if (graphics_system) {
    auto graphics_name = graphics_system->name();
    if (!graphics_name.empty()) {
      sb.Append(u8" <");
      sb.Append(graphics_name);
      sb.Append(u8">");
    }
  }

  if (Clock::guest_time_scalar() != 1.0) {
    sb.AppendFormat(u8" (@{:.2f}x)", Clock::guest_time_scalar());
  }

  if (initializing_shader_storage_) {
    sb.Append(u8" (Preloading shaders\u2026)");
  }

  patcher::Patcher* patcher = emulator()->patcher();
  if (patcher && patcher->IsAnyPatchApplied()) {
    sb.Append(u8" [Patches Applied]");
  }
  window_->SetTitle(sb.to_string_view());
}

void EmulatorWindow::SetInitializingShaderStorage(bool initializing) {
  if (initializing_shader_storage_ == initializing) {
    return;
  }
  initializing_shader_storage_ = initializing;
  UpdateTitle();
}

}  // namespace app
}  // namespace xe
