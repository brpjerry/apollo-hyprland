/**
 * @file External links used by the Web UI.
 *
 * Repository links point at the Apollo Hyprland fork. Apollo's how-to pages only exist in the upstream Apollo
 * wiki (the fork has no wiki pages), so wiki links keep pointing there.
 */

/** @brief GitHub `owner/name` of the fork this build comes from. */
export const FORK_REPOSITORY = 'brpjerry/apollo-hyprland'

/** @brief Web page of the fork's repository. */
export const FORK_URL = `https://github.com/${FORK_REPOSITORY}`

/** @brief Branch that holds the fork's license and notices. */
export const FORK_BRANCH = 'main'

/** @brief GitHub API endpoint listing the fork's releases. */
export const FORK_RELEASES_API = `https://api.github.com/repos/${FORK_REPOSITORY}/releases`

/** @brief Upstream Apollo wiki with the how-to pages (permissions, client commands, server commands...). */
export const APOLLO_WIKI_URL = 'https://github.com/ClassicOldSong/Apollo/wiki'

/** @brief Sunshine documentation, which still describes the app configuration Apollo inherits. */
export const SUNSHINE_DOCS_URL = 'https://docs.lizardbyte.dev/projects/sunshine/latest'

/**
 * @brief Build a link to an upstream Apollo wiki page.
 *
 * @param {string} page Wiki page name, e.g. `Permission-System`.
 * @return {string} Wiki page URL.
 */
export function apolloWikiPage(page) {
  return `${APOLLO_WIKI_URL}/${page}`
}

/**
 * @brief Build a link to a file in the fork's repository.
 *
 * @param {string} path Repository-relative file path.
 * @return {string} GitHub blob URL.
 */
export function forkFile(path) {
  return `${FORK_URL}/blob/${FORK_BRANCH}/${path}`
}
