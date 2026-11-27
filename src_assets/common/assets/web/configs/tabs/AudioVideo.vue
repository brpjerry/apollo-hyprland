<script setup>
import {computed, ref} from 'vue'
import {Info} from '@lucide/vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const props = defineProps({
  platform: String,
  config: Object,
  // Apollo: `vdisplayStatus` from GET /api/config; absent when the platform has no virtual display backend running.
  vdisplay: {
    type: [Number, String],
    default: undefined,
  },
})

const config = ref(props.config)

/** Virtual displays: the SudoVDA driver on Windows, Hyprland headless outputs on Linux. */
const vdisplaySupported = computed(() => props.platform === 'windows' || props.platform === 'linux')

/** SudoVDA driver states reported on Windows. */
const sudovdaStatus = {
  '1': 'Unknown',
  '0': 'Ready',
  '-1': 'Uninitialized',
  '-2': 'Version Incompatible',
  '-3': 'Watchdog Failed',
}
const sudovdaStatusText = computed(() => sudovdaStatus[String(props.vdisplay)] ?? sudovdaStatus['1'])
const vdisplayReady = computed(() => String(props.vdisplay) === '0')

/** Hyprland headless output states reported on Linux. */
const hyprlandStatusKeys = {
  '0': 'config.vdisplay_status_ready',
  '1': 'config.vdisplay_status_unknown',
  '-1': 'config.vdisplay_status_failed_linux',
}
const hyprlandStatusKey = computed(() => {
  if (props.vdisplay === undefined || props.vdisplay === null) {
    return 'config.vdisplay_status_unavailable_linux'
  }
  return hyprlandStatusKeys[String(props.vdisplay)] ?? 'config.vdisplay_status_unknown'
})

/** Fallback display mode format: `[Width]x[Height]x[FPS]`. */
const fallbackModeValid = computed(() => !config.value.fallback_mode || /^\d+x\d+x\d+(\.\d+)?$/.test(config.value.fallback_mode.trim()))
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #freebsd>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl list short sinks</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl list short sinks</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>


    <template v-if="platform === 'windows' || platform === 'linux'">
      <!-- Virtual Sink -->
      <div class="mb-3">
        <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
        <input type="text" class="form-control" id="virtual_sink"
               :placeholder="$t(platform === 'linux' ? 'config.virtual_sink_placeholder_linux' : 'config.virtual_sink_placeholder')"
               v-model="config.virtual_sink" />
        <div class="form-text pre-wrap">{{ $t(platform === 'linux' ? 'config.virtual_sink_desc_linux' : 'config.virtual_sink_desc') }}</div>
      </div>

      <!-- Install Steam Audio Drivers -->
      <Checkbox v-if="platform === 'windows'"
                class="mb-3"
                id="install_steam_audio_drivers"
                locale-prefix="config"
                v-model="config.install_steam_audio_drivers"
                default="true"
      ></Checkbox>

      <!-- Keep the virtual sink as default -->
      <Checkbox class="mb-3"
                id="keep_sink_default"
                locale-prefix="config"
                v-model="config.keep_sink_default"
                default="true"
      ></Checkbox>

      <!-- Capture the current default sink -->
      <Checkbox class="mb-3"
                id="auto_capture_sink"
                locale-prefix="config"
                v-model="config.auto_capture_sink"
                default="true"
      ></Checkbox>
    </template>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Externally Managed Audio -->
        <Checkbox class="mb-3"
                  id="external_audio"
                  locale-prefix="config"
                  v-model="config.external_audio"
                  default="false"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

    <!-- Fallback Display Mode -->
    <div class="mb-3">
      <label for="fallback_mode" class="form-label">{{ $t('config.fallback_mode') }}</label>
      <input type="text" class="form-control" id="fallback_mode" :class="{ 'is-invalid': !fallbackModeValid }"
             v-model="config.fallback_mode" placeholder="1920x1080x60" />
      <div class="invalid-feedback" v-if="!fallbackModeValid">{{ $t('config.fallback_mode_error') }}</div>
      <div class="form-text">{{ $t('config.fallback_mode_desc') }}</div>
    </div>

    <template v-if="vdisplaySupported">
      <!-- Headless Mode -->
      <Checkbox class="mb-3"
                id="headless_mode"
                locale-prefix="config"
                v-model="config.headless_mode"
                default="false"
      ></Checkbox>

      <!-- Double Refreshrate -->
      <Checkbox class="mb-3"
                id="double_refreshrate"
                locale-prefix="config"
                v-model="config.double_refreshrate"
                default="false"
      ></Checkbox>

      <!-- Isolated Virtual Display -->
      <Checkbox class="mb-3"
                id="isolated_virtual_display_option"
                locale-prefix="config"
                v-model="config.isolated_virtual_display_option"
                default="false"
      ></Checkbox>
    </template>

    <!-- SudoVDA driver status -->
    <template v-if="platform === 'windows'">
      <div class="alert" :class="vdisplayReady ? 'alert-success' : 'alert-warning'" id="vdisplay_status">
        <Info :size="18" class="icon"></Info> {{ $t('config.vdisplay_status_windows', { status: sudovdaStatusText }) }}
      </div>
      <div class="form-text mb-3" v-if="!vdisplayReady">{{ $t('config.vdisplay_status_hint_windows') }}</div>
    </template>

    <!-- Hyprland headless output status -->
    <template v-if="platform === 'linux'">
      <div class="alert" :class="vdisplayReady ? 'alert-success' : 'alert-warning'" id="vdisplay_status">
        <Info :size="18" class="icon"></Info> {{ $t('config.vdisplay_status_linux', { status: $t(hyprlandStatusKey) }) }}
      </div>
      <div class="form-text mb-3" v-if="!vdisplayReady">{{ $t('config.vdisplay_status_hint_linux') }}</div>
    </template>

  </div>
</template>
