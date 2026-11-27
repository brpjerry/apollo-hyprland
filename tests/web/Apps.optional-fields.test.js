import { beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/fetch_utils', () => ({
  apiFetch: vi.fn(() => Promise.resolve({ status: 204 })),
}))

import { apiFetch } from '../../src_assets/common/assets/web/fetch_utils'
import Apps, { APP_FIELD_DEFAULTS } from '../../src_assets/common/assets/web/Apps.vue'

beforeEach(() => vi.clearAllMocks())

/**
 * @brief Construct editing state without invoking Bootstrap or the network.
 * @param {object} app Application JSON to edit.
 * @return {object} Context for editApp() and save().
 */
function editContext(app) {
  return {
    apps: [app],
    platform: 'windows',
    canElevate: true,
    openEditModal: vi.fn(),
    closeEditModal: vi.fn(),
    loadApps: vi.fn(),
    errorText: vi.fn(async () => 'error'),
    addCmd: Apps.methods.addCmd,
    deleteCmd: Apps.methods.deleteCmd,
    $refs: { editModal: { querySelector: () => ({ scrollTop: 0 }) } },
    $t: key => key,
  }
}

describe('optional application fields', () => {
  const defaults = {
    'prep-cmd': [],
    'state-cmd': [],
    detached: [],
    'exclude-global-prep-cmd': false,
    'exclude-global-state-cmd': false,
    elevated: false,
    'auto-detach': true,
    'wait-all': true,
    'exit-timeout': 5,
    cmd: '',
    output: '',
    'working-dir': '',
    'image-path': '',
    gamepad: '',
    'allow-client-commands': true,
    'terminate-on-pause': false,
    'virtual-display': false,
    'use-app-identity': false,
    'per-client-app-identity': false,
    'scale-factor': 100,
  }

  // Bugs caught: 0, False positives: 1
  it.each([undefined, null])('defaults optional fields containing %s before editing and saving', value => {
    const app = { name: 'No cover' }
    if (value === null) {
      for (const key of Object.keys(defaults)) {
        app[key] = null
      }
    }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)

    expect(ctx.editForm).toEqual({ name: 'No cover', index: 0, ...defaults })
    expect(() => Apps.methods.addPrepCmd.call(ctx)).not.toThrow()
    expect(() => Apps.methods.addCmd.call(ctx, 'state')).not.toThrow()
    expect(ctx.editForm['state-cmd']).toEqual([{ do: '', undo: '', elevated: false }])
    expect(() => Apps.methods.addDetached.call(ctx)).not.toThrow()
    expect(() => Apps.methods.save.call(ctx)).not.toThrow()
    expect(apiFetch).toHaveBeenCalledOnce()
    expect(Object.values(JSON.parse(apiFetch.mock.calls[0][1].body))).not.toContain(null)
  })

  // Bugs caught: 0, False positives: 1
  it('preserves false, zero, and configured optional values', () => {
    const app = {
      name: 'Configured',
      ...defaults,
      'prep-cmd': [{ do: 'prepare', undo: 'restore', elevated: false }],
      'state-cmd': [{ do: 'resume', undo: 'pause', elevated: true }],
      detached: ['background'],
      'auto-detach': false,
      'wait-all': false,
      'exit-timeout': 0,
      cmd: 'launch --argument',
      output: '/logs/app.log',
      'working-dir': '/apps',
      'image-path': '/covers/cover.png',
      gamepad: 'ds5',
      'allow-client-commands': false,
      'terminate-on-pause': true,
      'virtual-display': true,
      'scale-factor': 150,
    }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)
    Apps.methods.save.call(ctx)

    expect(ctx.editForm).toEqual({ ...app, index: 0 })
    // An application without a UUID is still identified by its position.
    expect(JSON.parse(apiFetch.mock.calls[0][1].body)).toEqual({ ...app, index: 0 })
    expect(app).not.toHaveProperty('index')
  })
})

describe('command elevation flags', () => {
  // A missing flag left the Checkbox model undefined, so it toggled to the string "true", which the
  // state-command reader rejects.
  // Bugs caught: 0, False positives: 0
  it('turns missing or string elevation flags into booleans when editing', () => {
    const app = {
      name: 'Commands',
      'prep-cmd': [{ do: 'prepare', undo: '' }, { do: 'legacy', undo: '', elevated: 'true' }],
      'state-cmd': [{ do: 'resume', undo: 'pause' }, { do: 'other', undo: '', elevated: 'false' }],
    }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)

    expect(ctx.editForm['prep-cmd'].map(c => c.elevated)).toEqual([false, true])
    expect(ctx.editForm['state-cmd'].map(c => c.elevated)).toEqual([false, false])
  })

  // The server's app list reader takes 1 and "1" as elevated: an editor that read them as false would drop the
  // elevation when an unrelated field is saved.
  // Bugs caught: 0, False positives: 0
  it('keeps the elevation of commands flagged with 1 or "1"', () => {
    const app = {
      name: 'Commands',
      'prep-cmd': [{ do: 'a', undo: '', elevated: 1 }, { do: 'b', undo: '', elevated: '1' }, { do: 'c', undo: '', elevated: 2 }],
      'state-cmd': [{ do: 'd', undo: '', elevated: 1 }, { do: 'e', undo: '', elevated: 0 }],
    }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)

    expect(ctx.editForm['prep-cmd'].map(c => c.elevated)).toEqual([true, true, false])
    expect(ctx.editForm['state-cmd'].map(c => c.elevated)).toEqual([true, false])
  })
})

describe('UUID-based applications', () => {
  // Bugs caught: 0, False positives: 0
  it('saves an existing application by UUID without its list position', () => {
    const app = { name: 'Desktop', uuid: 'aaaa-bbbb', 'scale-factor': '120', 'exit-timeout': '7' }
    const ctx = editContext(app)

    Apps.methods.editApp.call(ctx, 0)
    Apps.methods.save.call(ctx)

    const body = JSON.parse(apiFetch.mock.calls[0][1].body)
    expect(apiFetch.mock.calls[0][0]).toBe('./api/apps')
    expect(body.uuid).toBe('aaaa-bbbb')
    expect(body).not.toHaveProperty('index')
    expect(body['scale-factor']).toBe(120)
    expect(body['exit-timeout']).toBe(7)
  })

  // Bugs caught: 0, False positives: 0
  it('says a save whose connection dropped is unconfirmed', async () => {
    apiFetch.mockRejectedValueOnce(new Error('offline'))
    const ctx = editContext({ name: 'Desktop', uuid: 'aaaa-bbbb' })

    Apps.methods.editApp.call(ctx, 0)
    await Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('apps.save_unconfirmed' + 'offline')
    expect(ctx.closeEditModal).not.toHaveBeenCalled()
  })

  // Bugs caught: 0, False positives: 0
  it('creates a new application without a UUID so the server assigns one', () => {
    const ctx = editContext({ name: 'unused' })

    Apps.methods.newApp.call(ctx)
    ctx.editForm.name = '  Steam  '
    Apps.methods.save.call(ctx)

    const body = JSON.parse(apiFetch.mock.calls[0][1].body)
    expect(body).not.toHaveProperty('uuid')
    expect(body.index).toBe(-1)
    expect(body.name).toBe('Steam')
    expect(body['state-cmd']).toEqual([])
    expect(body['allow-client-commands']).toBe(true)
  })

  // Bugs caught: 0, False positives: 0
  it('deletes by UUID through /api/apps/delete', async () => {
    apiFetch.mockResolvedValueOnce({ status: 200 })
    const ctx = {
      ...editContext({ name: 'Steam', uuid: 'cccc-dddd' }),
      closeDeleteModal: vi.fn(),
      $nextTick: vi.fn(),
    }

    Apps.methods.showDeleteModal.call(ctx, 0)
    expect(ctx.deleteTarget).toEqual({ index: 0, uuid: 'cccc-dddd', name: 'Steam' })
    await Apps.methods.confirmDelete.call(ctx)

    expect(apiFetch).toHaveBeenCalledExactlyOnceWith('./api/apps/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ uuid: 'cccc-dddd' }),
    })
    expect(ctx.closeDeleteModal).toHaveBeenCalledOnce()
    expect(ctx.loadApps).toHaveBeenCalledOnce()
  })

  // Bugs caught: 0, False positives: 0
  it('falls back to the position for an application saved before UUIDs existed', async () => {
    apiFetch.mockResolvedValueOnce({ status: 200 })
    const ctx = { ...editContext({ name: 'Legacy' }), closeDeleteModal: vi.fn() }
    ctx.deleteTarget = { index: 3, uuid: undefined, name: 'Legacy' }

    await Apps.methods.confirmDelete.call(ctx)

    expect(apiFetch.mock.calls[0][0]).toBe('./api/apps/3')
    expect(apiFetch.mock.calls[0][1].method).toBe('DELETE')
  })

  // Bugs caught: 0, False positives: 1
  it('reorders by UUID in the order shown', async () => {
    apiFetch.mockResolvedValueOnce({ status: 200 })
    const apps = [{ name: 'B', uuid: 'b' }, { name: 'A', uuid: 'a' }, { name: 'C', uuid: 'c' }]
    const ctx = {
      ...editContext(apps[0]),
      loadApps: vi.fn(async () => true),
      apps,
      displayedApps: [{ app: apps[1], index: 1 }, { app: apps[0], index: 0 }, { app: apps[2], index: 2 }],
      sortMode: 'asc',
    }

    await Apps.methods.saveOrder.call(ctx)

    expect(apiFetch).toHaveBeenCalledExactlyOnceWith('./api/apps/reorder', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ order: ['a', 'b', 'c'] }),
    })
    expect(ctx.sortMode).toBe('default')
    expect(ctx.loadApps).toHaveBeenCalledOnce()
    expect(ctx.actionDisabled).toBe(false)
  })

  // Bugs caught: 0, False positives: 0
  it('moves a dragged application and marks the order as changed', () => {
    const ctx = { apps: ['a', 'b', 'c', 'd'], orderChanged: false }

    Apps.methods.moveApp.call(ctx, 0, 2)
    expect(ctx.apps).toEqual(['b', 'c', 'a', 'd'])
    expect(ctx.orderChanged).toBe(true)

    Apps.methods.moveApp.call(ctx, 3, 0)
    expect(ctx.apps).toEqual(['d', 'b', 'c', 'a'])

    ctx.orderChanged = false
    Apps.methods.moveApp.call(ctx, 1, 1)
    Apps.methods.moveApp.call(ctx, 1, 9)
    expect(ctx.orderChanged).toBe(false)
  })

  // Bugs caught: 0, False positives: 0
  it('launches on the host by UUID after confirmation when opened locally', async () => {
    apiFetch.mockResolvedValueOnce({ status: 200 })
    vi.stubGlobal('confirm', vi.fn(() => true))
    const ctx = editContext({ name: 'Desktop', uuid: 'eeee' })

    await Apps.methods.launchApp.call(ctx, { name: 'Desktop', uuid: 'eeee' })

    expect(apiFetch).toHaveBeenCalledExactlyOnceWith('./api/apps/launch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ uuid: 'eeee' }),
    })
    expect(ctx.loadApps).toHaveBeenCalledOnce()
    vi.unstubAllGlobals()
  })

  // Bugs caught: 0, False positives: 0
  it('builds the Artemis launch link from the host and app identity', () => {
    const link = Apps.methods.launchLink.call(
      { hostUUID: 'host-1', hostName: 'My PC' },
      { uuid: 'app-1', name: 'Steam Big Picture' },
    )

    expect(link).toBe('art://launch?host_uuid=host-1&host_name=My+PC&app_uuid=app-1&app_name=Steam+Big+Picture')
  })

  // Bugs caught: 0, False positives: 0
  it('marks only the running application', () => {
    const ctx = { currentApp: 'run' }

    expect(Apps.methods.isRunning.call(ctx, { uuid: 'run' })).toBe(true)
    expect(Apps.methods.isRunning.call(ctx, { uuid: 'other' })).toBe(false)
    expect(Apps.methods.isRunning.call({ currentApp: '' }, {})).toBe(false)
  })

  // Bugs caught: 0, False positives: 1
  it('loads the running application and host identity with the list', async () => {
    apiFetch.mockResolvedValueOnce({
      status: 200,
      json: async () => ({ apps: [{ name: 'A', uuid: 'a' }], current_app: 'a', host_uuid: 'h', host_name: 'Host' }),
    })
    const ctx = { orderChanged: true }

    await Apps.methods.loadApps.call(ctx)

    expect(apiFetch).toHaveBeenCalledWith('./api/apps')
    expect(ctx).toMatchObject({ currentApp: 'a', hostUUID: 'h', hostName: 'Host', orderChanged: false })
    expect(ctx.apps).toEqual([{ name: 'A', uuid: 'a' }])
  })
})

describe('applications page', () => {
  // Bugs caught: 0, False positives: 1
  it('keeps cover URLs on the saved position while a dragged order is unsaved', async () => {
    apiFetch.mockResolvedValueOnce({
      status: 200,
      json: async () => ({ apps: [{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }, { name: 'Legacy' }] }),
    })
    const ctx = { orderChanged: false }
    await Apps.methods.loadApps.call(ctx)

    Apps.methods.moveApp.call(ctx, 0, 1)

    expect(Apps.methods.coverUrl.call(ctx, ctx.apps[0], 0)).toBe('/api/covers/1')
    expect(Apps.methods.coverUrl.call(ctx, ctx.apps[1], 1)).toBe('/api/covers/0')
    expect(Apps.methods.coverUrl.call(ctx, ctx.apps[2], 2)).toBe('/api/covers/2')
  })

  // Bugs caught: 0, False positives: 0
  it('renders the running indicator, launch link and Apollo app fields', async () => {
    const { mount, flushPromises } = await import('@vue/test-utils')
    apiFetch.mockImplementation(async url => ({
      status: 200,
      json: async () => (url === './api/apps'
        ? { apps: [{ name: 'Desktop', uuid: 'd' }, { name: 'Steam', uuid: 's' }], current_app: 'd', host_uuid: 'h', host_name: 'PC' }
        : { platform: 'linux' }),
    }))
    const wrapper = mount(Apps, {
      global: {
        mocks: { $t: key => key },
        stubs: { Checkbox: true, 'i18n-t': true },
      },
    })
    await flushPromises()

    const cards = wrapper.findAll('.app-card')
    expect(cards[0].text()).toContain('apps.running')
    expect(cards[0].find('a.btn-success').exists()).toBe(false)
    expect(cards[1].get('a.btn-success').attributes('href')).toBe('art://launch?host_uuid=h&host_name=PC&app_uuid=s&app_name=Steam')

    wrapper.vm.editForm = { name: 'Steam', index: 1, uuid: 's', ...structuredClone(APP_FIELD_DEFAULTS) }
    await flushPromises()
    for (const id of ['appGamepad', 'clientCommands', 'excludeGlobal_state', 'terminateOnPause', 'virtualDisplay', 'useAppIdentity', 'resolutionScaleFactor', 'appElevation']) {
      expect(wrapper.find(`#${id}`).exists(), id).toBe(true)
    }
    expect(wrapper.text()).toContain('APOLLO_CLIENT_UUID')
    wrapper.unmount()
    apiFetch.mockReset()
  })

  /**
   * @brief Mount the applications page with a list of applications.
   *
   * @param {object[]} apps Applications returned by /api/apps.
   * @param {Function} [answer] Response for any other URL, given the URL and options.
   * @return {Promise<object>} Wrapper.
   */
  async function mountApps(apps, answer = async () => ({ status: 200, json: async () => ({ platform: 'linux' }) })) {
    const { mount, flushPromises } = await import('@vue/test-utils')
    apiFetch.mockImplementation(async (url, options) => (url === './api/apps'
      ? { status: 200, json: async () => ({ apps: structuredClone(apps) }) }
      : answer(url, options)))
    const wrapper = mount(Apps, {
      global: {
        mocks: { $t: key => key },
        stubs: { Checkbox: true, 'i18n-t': true },
      },
    })
    await flushPromises()
    return wrapper
  }

  // Bugs caught: 0, False positives: 0
  it('keeps a dragged order that the server rejected and shows why on the list', async () => {
    const { flushPromises } = await import('@vue/test-utils')
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }], async url => (url === './api/apps/reorder'
      ? { status: 500, json: async () => ({ error: 'Disk full' }) }
      : { status: 200, json: async () => ({ platform: 'linux' }) }))

    wrapper.vm.moveApp(0, 1)
    await wrapper.vm.saveOrder()
    await flushPromises()

    expect(wrapper.vm.apps.map(app => app.name)).toEqual(['B', 'A'])
    expect(wrapper.vm.canSaveOrder).toBe(true)
    const alerts = wrapper.findAll('.alert-danger').filter(alert => !alert.element.closest('.modal'))
    expect(alerts.map(alert => alert.text()).join()).toContain('Disk full')
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // Bugs caught: 0, False positives: 0
  it('keeps the list and the dragged order when reloading the apps fails', async () => {
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }])
    wrapper.vm.moveApp(0, 1)
    apiFetch.mockResolvedValueOnce({ status: 500, json: async () => ({ error: 'apps.json unreadable' }) })

    await wrapper.vm.loadApps()

    expect(wrapper.vm.apps.map(app => app.name)).toEqual(['B', 'A'])
    expect(wrapper.vm.orderChanged).toBe(true)
    expect(wrapper.vm.actionError).toContain('apps.json unreadable')

    apiFetch.mockRejectedValueOnce(new Error('offline'))
    await wrapper.vm.loadApps()
    expect(wrapper.vm.apps.map(app => app.name)).toEqual(['B', 'A'])
    expect(wrapper.vm.actionError).toContain('offline')
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // Bugs caught: 0, False positives: 0
  it('keeps the name-sorted view when the reload after saving that order fails', async () => {
    const wrapper = await mountApps([{ name: 'B', uuid: 'b' }, { name: 'A', uuid: 'a' }])
    wrapper.vm.sortMode = 'asc'
    apiFetch.mockImplementation(async url => (url === './api/apps'
      ? { status: 500, json: async () => ({ error: 'apps.json unreadable' }) }
      : { status: 200, json: async () => ({}) }))

    await wrapper.vm.saveOrder()

    expect(wrapper.vm.sortMode).toBe('asc')
    expect(wrapper.vm.displayedApps.map(({ app }) => app.name)).toEqual(['A', 'B'])
    expect(wrapper.vm.actionError).toContain('apps.json unreadable')
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // A delete moves every later app down one position on the server, and apps without a UUID are acted on by
  // position, so the old list must not stay usable when it cannot be reloaded.
  // Bugs caught: 0, False positives: 0
  it('drops a list of positional apps when it cannot be reloaded after a delete', async () => {
    const { flushPromises } = await import('@vue/test-utils')
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'Legacy 1' }, { name: 'Legacy 2' }])
    let answerDelete
    apiFetch.mockImplementation(url => (url === './api/apps/delete'
      ? new Promise(resolve => { answerDelete = resolve })
      : Promise.resolve({ status: 500, json: async () => ({ error: 'apps.json unreadable' }) })))

    wrapper.vm.deleteTarget = { index: 0, uuid: 'a', name: 'A' }
    const deleting = wrapper.vm.confirmDelete()
    await flushPromises()
    expect(wrapper.vm.actionDisabled).toBe(true)

    answerDelete({ status: 200 })
    await deleting
    expect(wrapper.vm.apps).toEqual([])
    expect(wrapper.vm.actionError).toContain('apps.json unreadable')
    expect(wrapper.vm.actionDisabled).toBe(false)
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // Saving the deleted app from a list that still shows it would create it again.
  // Bugs caught: 0, False positives: 0
  it('keeps the other UUID apps but not the deleted one when the list cannot be reloaded after a delete', async () => {
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }])
    apiFetch.mockImplementation(async url => (url === './api/apps/delete'
      ? { status: 200 }
      : { status: 500, json: async () => ({ error: 'apps.json unreadable' }) }))

    wrapper.vm.deleteTarget = { index: 0, uuid: 'a', name: 'A' }
    await wrapper.vm.confirmDelete()

    expect(wrapper.vm.apps.map(app => app.name)).toEqual(['B'])
    expect(wrapper.vm.actionError).toContain('apps.json unreadable')
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // The server writes the shortened list before it can fail (and a proxy can fail after the write): a retry against
  // the old positions would delete the next app.
  // Bugs caught: 0, False positives: 0
  it('reloads the list after a delete that reported an error', async () => {
    const wrapper = await mountApps([{ name: 'Legacy 1' }, { name: 'Legacy 2' }])
    apiFetch.mockImplementation(async url => (url === './api/apps/0'
      ? { status: 502, json: async () => ({ error: 'Bad gateway' }) }
      : { status: 200, json: async () => ({ apps: [{ name: 'Legacy 2' }] }) }))

    wrapper.vm.deleteTarget = { index: 0, uuid: undefined, name: 'Legacy 1' }
    await wrapper.vm.confirmDelete()

    expect(wrapper.vm.apps.map(app => app.name)).toEqual(['Legacy 2'])
    expect(wrapper.vm.actionError).toContain('Bad gateway')

    // When that reload fails too, the positional list is dropped.
    apiFetch.mockImplementation(async url => (url === './api/apps/0'
      ? { status: 502, json: async () => ({ error: 'Bad gateway' }) }
      : { status: 500, json: async () => ({ error: 'apps.json unreadable' }) }))
    wrapper.vm.deleteTarget = { index: 0, uuid: undefined, name: 'Legacy 2' }
    await wrapper.vm.confirmDelete()
    expect(wrapper.vm.apps).toEqual([])
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // Bugs caught: 0, False positives: 0
  it('requests covers by the saved order after a reorder whose reload failed', async () => {
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }])
    wrapper.vm.moveApp(0, 1)
    apiFetch.mockImplementation(async url => (url === './api/apps'
      ? { status: 500, json: async () => ({ error: 'apps.json unreadable' }) }
      : { status: 200, json: async () => ({}) }))

    await wrapper.vm.saveOrder()

    expect(wrapper.vm.apps.map(app => app.name)).toEqual(['B', 'A'])
    expect(wrapper.vm.coverUrl(wrapper.vm.apps[0], 0)).toBe('/api/covers/0')
    expect(wrapper.vm.coverUrl(wrapper.vm.apps[1], 1)).toBe('/api/covers/1')
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // A dropped connection used to escape the action without a message, and the grid unlocked before its refresh
  // finished, so that older list could overwrite a later action's result.
  // Bugs caught: 0, False positives: 0
  it.each([
    ['launch', './api/apps/launch', 'apps.launch_failed'],
    ['close', './api/apps/close', 'apps.close_failed'],
  ])('reports a %s request that never reached the server and stays locked until the list is back', async (action, actionUrl, message) => {
    const { flushPromises } = await import('@vue/test-utils')
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }])
    vi.stubGlobal('confirm', vi.fn(() => true))
    let answerList
    apiFetch.mockImplementation(url => {
      if (url === actionUrl) return Promise.reject(new Error('offline'))
      if (url === './api/apps') return new Promise(resolve => { answerList = resolve })
      return Promise.resolve({ status: 200, json: async () => ({}) })
    })

    const done = action === 'launch' ? wrapper.vm.launchApp({ name: 'A', uuid: 'a' }) : wrapper.vm.closeApp()
    await flushPromises()
    expect(wrapper.vm.actionError).toContain(message)
    expect(wrapper.vm.actionError).toContain('offline')
    expect(wrapper.vm.actionDisabled).toBe(true)

    answerList({ status: 200, json: async () => ({ apps: [{ name: 'A', uuid: 'a' }] }) })
    await done
    expect(wrapper.vm.actionDisabled).toBe(false)
    vi.unstubAllGlobals()
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // Bugs caught: 0, False positives: 0
  it('reports a reorder request that never reached the server', async () => {
    const { flushPromises } = await import('@vue/test-utils')
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }], async url => {
      if (url === './api/apps/reorder') throw new Error('offline')
      return { status: 200, json: async () => ({ platform: 'linux' }) }
    })

    wrapper.vm.moveApp(0, 1)
    await wrapper.vm.saveOrder()
    await flushPromises()

    expect(wrapper.vm.actionError).toContain('offline')
    expect(wrapper.vm.actionDisabled).toBe(false)
    expect(wrapper.vm.orderChanged).toBe(true)
    wrapper.unmount()
    apiFetch.mockReset()
  })

  // Dragging an application without a UUID would change the position its edit and delete requests use.
  // Bugs caught: 0, False positives: 0
  it('offers no reordering while an application has no UUID', async () => {
    const wrapper = await mountApps([{ name: 'A', uuid: 'a' }, { name: 'B', uuid: 'b' }, { name: 'Legacy' }])

    expect(wrapper.vm.dragEnabled).toBe(false)
    wrapper.vm.sortMode = 'asc'
    expect(wrapper.vm.canSaveOrder).toBe(false)
    wrapper.unmount()
    apiFetch.mockReset()
  })
})
