import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'

import NvidiaNvencEncoder from '../../src_assets/common/assets/web/configs/tabs/encoders/NvidiaNvencEncoder.vue'
import configTabs from '../../src_assets/common/assets/web/configs/config_tabs.json'

function mountNvenc(platform) {
  const config = structuredClone(configTabs.find(tab => tab.id === 'nv').options)
  const wrapper = mount(NvidiaNvencEncoder, {
    props: { platform, config },
    global: {
      mocks: { $t: key => key },
    },
  })
  return { wrapper, config }
}

describe('NVENC encoder settings', () => {
  // Bugs caught: 0, False positives: 0
  it('offers split frame encoding on Linux, whose native encoder supports it', async () => {
    const { wrapper, config } = mountNvenc('linux')
    const select = wrapper.get('#nvenc_split_encode')
    expect(select.element.value).toBe(config.nvenc_split_encode)
    await select.setValue('enabled')
    expect(config.nvenc_split_encode).toBe('enabled')
    // The other Windows-only settings stay hidden.
    expect(wrapper.find('#nvenc_realtime_hags').exists()).toBe(false)
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('keeps split frame encoding hidden on other platforms', () => {
    const { wrapper } = mountNvenc('macos')
    expect(wrapper.find('#nvenc_split_encode').exists()).toBe(false)
    wrapper.unmount()
  })
})
