import { FORK_RELEASES_API } from './links'

/** @brief Number of numeric version components compared (Apollo Hyprland tags look like `v0.4.8.1`). */
const NUMERIC_PARTS = 4

/** @brief Rank of each pre-release channel; a final release ranks above all of them. */
const PRERELEASE_RANKS = { alpha: 0, beta: 1, rc: 2 }
const FINAL_RANK = 3

/**
 * @brief Parsed Apollo release version and comparison helpers.
 *
 * Kept under the Sunshine name and file because the pages import it as `sunshine_version`. It understands the
 * fork's release tags: up to four numeric parts (`v0.4.8.1`) and an optional pre-release suffix (`v0.4.9-beta.2`).
 */
class SunshineVersion {
  /**
   * @brief Construct a version from GitHub release data or a version string.
   *
   * @param {?object} release GitHub release data.
   * @param {?string} version Version string when release data is unavailable.
   */
  constructor(release = null, version = null) {
    if (release) {
      this.release = release;
      this.version = release.tag_name;
      this.versionName = release.name;
      this.versionTag = release.tag_tag;
    } else if (version) {
      this.release = null;
      this.version = version;
      this.versionName = null;
      this.versionTag = null;
    } else {
      throw new Error('Either release or version must be provided');
    }
    this.versionParts = this.parseVersion(this.version);
    this.prerelease = this.parsePrerelease(this.version);
    this.versionMajor = this.versionParts ? this.versionParts[0] : null;
    this.versionMinor = this.versionParts ? this.versionParts[1] : null;
    this.versionPatch = this.versionParts ? this.versionParts[2] : null;
    this.versionIncremental = this.versionParts ? (this.versionParts[3] ?? null) : null;
  }

  /**
   * @brief Split a version string into its numeric components.
   *
   * @param {string} version Version string to parse.
   * @return {?number[]} Numeric version components, without the pre-release suffix.
   */
  parseVersion(version) {
    if (!version) {
      return null;
    }
    let v = version;
    if (v.startsWith("v")) {
      v = v.substring(1);
    }
    return v.split('-')[0].split('.').map(Number);
  }

  /**
   * @brief Parse the pre-release suffix of a version string.
   *
   * @param {string} version Version string to parse.
   * @return {?{rank: number, number: number}} Pre-release channel rank and number, or null for a final release.
   */
  parsePrerelease(version) {
    if (!version || !version.includes('-')) {
      return null;
    }
    const [channel, number] = version.substring(version.indexOf('-') + 1).split('.');
    const rank = PRERELEASE_RANKS[channel.toLowerCase()];
    if (rank === undefined) {
      return null;
    }
    return { rank, number: Number.parseInt(number, 10) || 0 };
  }

  /**
   * @brief Compare this version with another version.
   *
   * Compares up to four numeric parts (missing parts count as 0). With equal numbers, a final release is greater
   * than a pre-release, and pre-releases compare by channel (alpha < beta < rc) and number.
   *
   * @param {SunshineVersion|string} otherVersion Version to compare.
   * @return {boolean} Whether this version is greater.
   */
  isGreater(otherVersion) {
    let other;
    if (otherVersion instanceof SunshineVersion) {
      other = otherVersion;
    } else if (typeof otherVersion === 'string') {
      other = new SunshineVersion(null, otherVersion);
    } else {
      throw new TypeError('Invalid argument: otherVersion must be a SunshineVersion object or a version string');
    }

    const parts = this.versionParts;
    const otherParts = other.versionParts;
    if (!parts || !otherParts || parts.some(Number.isNaN) || otherParts.some(Number.isNaN)) {
      return false;
    }
    for (let i = 0; i < NUMERIC_PARTS; i++) {
      const part = parts[i] ?? 0;
      const otherPart = otherParts[i] ?? 0;
      if (part !== otherPart) {
        return part > otherPart;
      }
    }

    const rank = this.prerelease?.rank ?? FINAL_RANK;
    const otherRank = other.prerelease?.rank ?? FINAL_RANK;
    if (rank !== otherRank) {
      return rank > otherRank;
    }
    return (this.prerelease?.number ?? 0) > (other.prerelease?.number ?? 0);
  }
}

/**
 * @brief Whether a GitHub release is one of the fork's versioned releases (tags `v*`).
 *
 * @param {?object} release GitHub release data.
 * @return {boolean} Whether the release has a `v*` tag.
 */
export function isVersionedRelease(release) {
  return typeof release?.tag_name === 'string' && release.tag_name.startsWith('v');
}

/**
 * @brief Fetch the fork's highest stable release and highest pre-release from GitHub.
 *
 * The highest version wins, not the most recently published one: a maintenance release of an older branch can be
 * published after a newer version. Only the first page of releases (GitHub's 30 most recent) is read.
 *
 * @param {Function} [fetchJson] Loader returning parsed JSON for a URL.
 * @return {Promise<{stable: ?SunshineVersion, preRelease: ?SunshineVersion}>} Release versions, null when absent.
 */
export async function fetchReleaseVersions(fetchJson = async url => (await fetch(url)).json()) {
  const releases = await fetchJson(FORK_RELEASES_API);
  const newest = { stable: null, preRelease: null };
  for (const release of Array.isArray(releases) ? releases : []) {
    if (!isVersionedRelease(release) || release.draft) {
      continue;
    }
    const version = new SunshineVersion(release);
    if (version.versionParts.some(Number.isNaN)) {
      continue;
    }
    const channel = release.prerelease ? 'preRelease' : 'stable';
    if (!newest[channel] || version.isGreater(newest[channel])) {
      newest[channel] = version;
    }
  }
  return newest;
}

export default SunshineVersion;
