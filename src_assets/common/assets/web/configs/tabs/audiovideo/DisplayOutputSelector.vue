<script setup>
import { ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
const outputNamePlaceholders = {
  windows: '{de9bb7e2-186e-505b-9e93-f48793333810}',
  linux: 'DP-1',
}
const outputNamePlaceholder = outputNamePlaceholders[props.platform] ?? '0'
</script>

<template>
  <div class="mb-3">
    <label for="output_name" class="form-label">{{ $tp('config.output_name') }}</label>
    <input type="text" class="form-control" id="output_name" :placeholder="outputNamePlaceholder"
           v-model="config.output_name"/>
    <div class="form-text">
      {{ $tp('config.output_name_desc') }}<br>
      <PlatformLayout :platform="platform">
        <template #windows>
          <pre style="white-space: pre-line;">
            <b>&nbsp;&nbsp;{</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"display_name": "\\\\.\\DISPLAY1"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"friendly_name": "ROG PG279Q"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;...</b>
            <b>&nbsp;&nbsp;}</b>
          </pre>
        </template>
        <template #linux>
          <pre style="white-space: pre-line;">
            Info: -------- Start of Wayland monitor list --------
            Info: Monitor 0 is <b>DP-1</b>: Dell Inc. DELL U2720Q (3840x2160, logical 2560x1440 at 0,0, scale 1.5, transform 0)
            Info: Monitor 1 is <b>HDMI-A-1</b>: LG Electronics LG TV (1920x1080, logical 1920x1080 at 2560,0, scale 1, transform 0)
            Info: --------- End of Wayland monitor list ---------
          </pre>
        </template>
        <template #macos>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: Monitor-0 (id: 3) connected: true
            Info: Detected display: Monitor-1 (id: 2) connected: true
          </pre>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
