/**
 * @file tests/unit/test_config_catalog_contract.cpp
 * @brief Portable unit contract for configure-time configuration catalogs.
 */
#include "../tests_common.h"

#include "config_catalog_contract.generated.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

  using option_set = std::set<std::string, std::less<>>;

  template<typename Range>
  option_set as_option_set(const Range &values) {
    option_set options;
    for (const std::string_view value : values) {
      options.emplace(value);
    }
    return options;
  }

  std::vector<std::string> missing_public_contracts(
    const option_set &config_options,
    const option_set &documented_options,
    const option_set &translated_options) {
    const option_set internal_options {
      "flags",  // Internal config flags, not user-configurable.
      "rtss_disable_vsync_ullm",  // Legacy alias for frame_limiter_disable_vsync.
      "dd_vdd_static_monitor_count"  // Legacy alias for dd_virtual_display_permanent_count.
    };

    std::vector<std::string> missing;
    for (const auto &option : config_options) {
      if (internal_options.contains(option)) {
        continue;
      }
      if (!documented_options.contains(option)) {
        missing.emplace_back("configuration.md missing: " + option);
      }
      if (!translated_options.contains(option)) {
        missing.emplace_back("en.json missing: " + option);
      }
    }
    return missing;
  }



// Options with no field on the v2 settings page, frozen as of the audit that
// added this test.  Reaching one of these means hand-editing sunshine.conf,
// which is not something a host owner should have to do — but retrofitting
// eighty fields is a separate job, so the list is a ratchet rather than a fix:
// nothing new may join it without a deliberate edit here.
//
// Adding a config option?  Give it a field in
// src_assets/common/assets/web/configs/settingsSchema.ts.  Only add it below if
// it genuinely has no business on the settings page, and say why.
const option_set &options_without_ui_baseline() {
  static const option_set baseline {
      "adapter_pnp_id",
      "amd_av1_latency_mode",
      "amd_av1_screen_content",
      "amd_coder",
      "amd_enforce_hrd",
      "amd_high_motion_quality_boost",
      "amd_input_queue_size",
      "amd_lowlatency_mode",
      "amd_ltr_frames",
      "amd_preanalysis",
      "amd_qvbr_quality_level",
      "amd_rc",
      "amd_smart_access_video",
      "amd_usage",
      "amd_vbaq",
      "auto_capture_sink",
      "back_button_timeout",
      "credentials_file",
      "dd_config_revert_delay",
      "dd_mode_remapping",
      "dd_snapshot_exclude_devices",
      "dd_wa_hdr_toggle",
      "dd_wa_hdr_toggle_delay",
      "enable_discovery",
      "enable_input_only_mode",
      "enable_pairing",
      "envvar_compatibility_mode",
      "fallback_mode",
      "file_state",
      "forward_rumble",
      "gamepad",
      "global_prep_cmd",
      "global_state_cmd",
      "hide_tray_controls",
      "ignore_encoder_probe_failure",
      "keep_sink_default",
      "key_rightalt_to_key_win",
      "keybindings",
      "legacy_ordering",
      "limit_framerate",
      "locale",
      "lossless_scaling_path",
      "min_threads",
      "nvenc_h264_cavlc",
      "nvenc_intra_refresh",
      "nvenc_latency_over_power",
      "nvenc_opengl_vulkan_on_dxgi",
      "nvenc_realtime_hags",
      "nvenc_spatial_aq",
      "nvenc_split_encode",
      "nvenc_temporal_aq",
      "nvenc_twopass",
      "nvenc_vbv_increase",
      "pacing_max_bitrate_kbps",
      "packetsize",
      "qsv_coder",
      "qsv_slow_hevc",
      "remember_me_refresh_token_ttl_seconds",
      "rtss_install_path",
      "rtx_hdr",
      "rtx_hdr_contrast",
      "rtx_hdr_force_sdr",
      "rtx_hdr_middle_gray",
      "rtx_hdr_peak_brightness",
      "rtx_hdr_saturation",
      "rtx_hdr_sdr_brightness",
      "server_cmd",
      "session_token_ttl_seconds",
      "sw_preset",
      "sw_tune",
      "update_check_interval",
      "vaapi_strict_rc_buffer",
      "vibeshine_file_state",
      "vk_rc_mode",
      "vk_tune",
      "vt_coder",
      "vt_realtime",
      "vt_software",
  };
  return baseline;
}

}  // namespace

TEST(ConfigConsistency, PublicConfigOptionsAreDocumentedAndTranslated) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto documented_options = as_option_set(sunshine::test::config_catalog_contract::documented_options);
  const auto translated_options = as_option_set(sunshine::test::config_catalog_contract::translated_options);

  ASSERT_FALSE(config_options.empty());
  ASSERT_FALSE(documented_options.empty());
  ASSERT_FALSE(translated_options.empty());

  const auto missing = missing_public_contracts(config_options, documented_options, translated_options);
  ASSERT_TRUE(missing.empty()) << [&] {
    std::string message = "Public config options missing from retained contracts:\n";
    for (const auto &entry : missing) {
      message += "  " + entry + '\n';
    }
    return message;
  }();
}

TEST(ConfigConsistency, DummyOptionsAreAbsentFromRetainedContracts) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto documented_options = as_option_set(sunshine::test::config_catalog_contract::documented_options);
  const auto translated_options = as_option_set(sunshine::test::config_catalog_contract::translated_options);
  const std::vector<std::string_view> dummy_options {
    "dummy_config_option",
    "nonexistent_setting",
    "fake_config_parameter",
    "test_dummy_option",
    "invalid_config_key"
  };

  for (const auto option : dummy_options) {
    EXPECT_FALSE(config_options.contains(option)) << option;
    EXPECT_FALSE(documented_options.contains(option)) << option;
    EXPECT_FALSE(translated_options.contains(option)) << option;
  }
}


TEST(ConfigConsistency, NewPublicConfigOptionsAreReachableFromTheSettingsPage) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto ui_options = as_option_set(sunshine::test::config_catalog_contract::ui_options);
  const option_set internal_options {
    "flags",
    "rtss_disable_vsync_ullm",
    "dd_vdd_static_monitor_count"
  };

  ASSERT_FALSE(config_options.empty());
  ASSERT_FALSE(ui_options.empty());

  std::vector<std::string> unreachable;
  for (const auto &option : config_options) {
    if (internal_options.contains(option) || ui_options.contains(option)) {
      continue;
    }
    if (!options_without_ui_baseline().contains(option)) {
      unreachable.emplace_back(option);
    }
  }

  EXPECT_TRUE(unreachable.empty()) << [&] {
    std::string message =
      "Config options with no field in web/configs/settingsSchema.ts.\n"
      "Setting these means hand-editing sunshine.conf.  Add a field to the\n"
      "settings schema, or add the option to options_without_ui_baseline()\n"
      "with a reason:\n";
    for (const auto &entry : unreachable) {
      message += "  " + entry + '\n';
    }
    return message;
  }();
}

TEST(ConfigConsistency, TheNoUiBaselineDoesNotRot) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto ui_options = as_option_set(sunshine::test::config_catalog_contract::ui_options);

  // An entry that has since gained a field, or stopped being an option at all,
  // is stale.  Left alone the baseline would quietly grow into a list nobody
  // trusts, so it has to shrink as the settings page catches up.
  for (const auto &option : options_without_ui_baseline()) {
    EXPECT_TRUE(config_options.contains(option))
      << option << " is in the no-UI baseline but is no longer a config option; remove it.";
    EXPECT_FALSE(ui_options.contains(option))
      << option << " now has a settings field; remove it from the no-UI baseline.";
  }
}

TEST(ConfigConsistency, SettingsPageOnlyOffersRealConfigOptions) {
  const auto config_options = as_option_set(sunshine::test::config_catalog_contract::source_options);
  const auto ui_options = as_option_set(sunshine::test::config_catalog_contract::ui_options);

  // The mirror of the check above: a field whose key config.cpp never reads
  // writes a line to sunshine.conf that does nothing, and the page shows a
  // control that silently has no effect.
  for (const auto &option : ui_options) {
    EXPECT_TRUE(config_options.contains(option))
      << option << " has a settings field but is not parsed in config.cpp.";
  }
}
