import { describe, expect, it, vi } from 'vitest'

import SunshineVersion, { fetchReleaseVersions } from '../../src_assets/common/assets/web/sunshine_version.js'

describe('SunshineVersion', () => {
  it('parses release metadata', () => {
    const version = new SunshineVersion({
      name: 'Sunshine release',
      tag_name: 'v2026.9.16',
      tag_tag: 'stable',
    })

    expect(version.release).toBeDefined()
    expect(version.version).toBe('v2026.9.16')
    expect(version.versionName).toBe('Sunshine release')
    expect(version.versionTag).toBe('stable')
    expect(version.versionParts).toEqual([2026, 9, 16])
    expect(version.versionMajor).toBe(2026)
    expect(version.versionMinor).toBe(9)
    expect(version.versionPatch).toBe(16)
  })

  it('parses a version string without release metadata', () => {
    const version = new SunshineVersion(null, '1.2.3')

    expect(version.release).toBeNull()
    expect(version.versionParts).toEqual([1, 2, 3])
    expect(version.versionName).toBeNull()
    expect(version.versionTag).toBeNull()
  })

  it('requires either release metadata or a version string', () => {
    expect(() => new SunshineVersion()).toThrow('Either release or version must be provided')
  })

  it('compares version objects and strings', () => {
    const version = new SunshineVersion(null, '2.0.0')

    expect(version.isGreater('1.9.9')).toBe(true)
    expect(version.isGreater(new SunshineVersion(null, '2.0.0'))).toBe(false)
    expect(version.isGreater('3.0.0')).toBe(false)
  })

  it('handles absent and invalid comparison values', () => {
    const version = new SunshineVersion(null, '2.0.0')
    const unparsedVersion = new SunshineVersion(null, 'placeholder')
    unparsedVersion.versionParts = null

    expect(version.parseVersion(null)).toBeNull()
    expect(unparsedVersion.isGreater('1.0.0')).toBe(false)
    expect(() => version.isGreater(2)).toThrow(TypeError)
  })
})

describe('Apollo Hyprland release versions', () => {
  // Bugs caught: 0, False positives: 0
  it('compares the fourth version part of fork tags', () => {
    const installed = new SunshineVersion(null, '0.4.8.1')

    expect(installed.versionParts).toEqual([0, 4, 8, 1])
    expect(installed.versionIncremental).toBe(1)
    expect(new SunshineVersion(null, 'v0.4.8.2').isGreater(installed)).toBe(true)
    expect(installed.isGreater('v0.4.8')).toBe(true)
    expect(installed.isGreater('0.4.8.1')).toBe(false)
  })

  // Bugs caught: 0, False positives: 0
  it('orders pre-releases below the final release of the same version', () => {
    const beta = new SunshineVersion(null, 'v0.4.9-beta.2')

    expect(beta.versionParts).toEqual([0, 4, 9])
    expect(beta.isGreater('0.4.8.1')).toBe(true)
    expect(beta.isGreater('0.4.9-beta.1')).toBe(true)
    expect(beta.isGreater('0.4.9-alpha.7')).toBe(true)
    expect(beta.isGreater('0.4.9')).toBe(false)
    expect(new SunshineVersion(null, '0.4.9').isGreater(beta)).toBe(true)
  })

  // Bugs caught: 0, False positives: 1
  it('finds the newest stable release and pre-release among v* tags', async () => {
    const fetchJson = vi.fn(async () => [
      { tag_name: 'nightly', prerelease: true },
      { tag_name: 'v0.5.0-beta.1', prerelease: true },
      { tag_name: 'v0.4.9', prerelease: false, draft: true },
      { tag_name: 'v0.4.8.1', prerelease: false },
      { tag_name: 'v0.4.8', prerelease: false },
    ])

    const { stable, preRelease } = await fetchReleaseVersions(fetchJson)

    expect(fetchJson).toHaveBeenCalledExactlyOnceWith('https://api.github.com/repos/brpjerry/apollo-hyprland/releases?per_page=100&page=1')
    expect(stable.version).toBe('v0.4.8.1')
    expect(preRelease.version).toBe('v0.5.0-beta.1')
  })

  // Bugs caught: 0, False positives: 0
  it('picks the highest version in each channel, whatever the publication order', async () => {
    const fetchJson = vi.fn(async () => [
      { tag_name: 'v0.4.7.5', prerelease: false },
      { tag_name: 'v0.4.9-alpha.1', prerelease: true },
      { tag_name: 'v0.4.8.1', prerelease: false },
      { tag_name: 'v0.4.9-beta.1', prerelease: true },
      { tag_name: 'vnext', prerelease: false },
    ])

    const { stable, preRelease } = await fetchReleaseVersions(fetchJson)

    expect(stable.version).toBe('v0.4.8.1')
    expect(preRelease.version).toBe('v0.4.9-beta.1')
  })

  // Bugs caught: 0, False positives: 0
  it('reads further pages while GitHub returns full ones', async () => {
    const full = Array.from({ length: 100 }, (_, i) => ({ tag_name: `v0.3.${i}`, prerelease: false }))
    const fetchJson = vi.fn(async url => (url.endsWith('page=2') ? [{ tag_name: 'v0.4.8.2', prerelease: false }] : full))

    const { stable } = await fetchReleaseVersions(fetchJson)

    expect(fetchJson.mock.calls.map(([url]) => url)).toEqual([
      'https://api.github.com/repos/brpjerry/apollo-hyprland/releases?per_page=100&page=1',
      'https://api.github.com/repos/brpjerry/apollo-hyprland/releases?per_page=100&page=2',
    ])
    expect(stable.version).toBe('v0.4.8.2')
  })

  // Bugs caught: 0, False positives: 0
  it('returns no versions when a later page fails', async () => {
    const full = Array.from({ length: 100 }, (_, i) => ({ tag_name: `v0.3.${i}`, prerelease: false }))
    const fetchJson = vi.fn(async url => (url.endsWith('page=2') ? { message: 'API rate limit exceeded' } : full))

    expect(await fetchReleaseVersions(fetchJson)).toEqual({ stable: null, preRelease: null })
  })

  // Bugs caught: 0, False positives: 0
  it('returns no versions when GitHub answers with an error object', async () => {
    const { stable, preRelease } = await fetchReleaseVersions(async () => ({ message: 'Not Found' }))

    expect(stable).toBeNull()
    expect(preRelease).toBeNull()
  })
})
