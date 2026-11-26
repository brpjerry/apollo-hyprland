<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <div class="my-4">
      <h1>{{ $t('apps.applications_title') }}<span v-if="apps.length"> ({{ appCountLabel }})</span></h1>
      <p class="mb-1">{{ $t('apps.applications_desc') }}</p>
      <p class="mb-1">{{ $t('apps.applications_reorder_desc') }}</p>
      <p class="pre-wrap text-muted small">{{ $t('apps.applications_tips') }}</p>
    </div>

    <!-- Actions toolbar -->
    <div class="toolbar apps-toolbar d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3">
      <!-- Left side actions -->
      <div class="d-flex align-items-center gap-2">
        <!-- Add new application -->
        <button class="btn btn-primary" @click="newApp" :disabled="actionDisabled">
          <layers-plus :size="18" class="icon"></layers-plus>
          {{ $t('apps.add_new') }}
        </button>
        <!-- Save the dragged or name-sorted order -->
        <button class="btn btn-warning" v-if="canSaveOrder" @click="saveOrder" :disabled="actionDisabled">
          <save :size="18" class="icon"></save>
          {{ $t('apps.save_order') }}
        </button>
      </div>
      <!-- Right side actions -->
      <div class="d-flex align-items-center gap-2" v-if="apps && apps.length > 0">
        <!-- Sort by name toggle -->
        <button class="btn btn-outline-secondary text-nowrap" type="button" @click="toggleSort"
                :class="{ active: sortMode !== 'default' }"
                :aria-pressed="sortMode !== 'default'"
                :title="$t('apps.sort_by_name') + ': ' + sortModeLabel">
          <arrow-up-down v-if="sortMode === 'default'" :size="16" class="icon me-1"></arrow-up-down>
          <arrow-up v-else-if="sortMode === 'asc'" :size="16" class="icon me-1"></arrow-up>
          <arrow-down v-else :size="16" class="icon me-1"></arrow-down>
          {{ $t('apps.sort_by_name') }}
        </button>
        <!-- Search box -->
        <div class="input-group">
          <label for="app-search" class="visually-hidden">{{ $t('apps.search_placeholder') }}</label>
          <input id="app-search" type="text" class="form-control" v-model="searchQuery" :placeholder="$t('apps.search_placeholder')" />
          <button v-if="searchQuery" class="btn btn-outline-secondary" type="button" @click="resetSearchQuery" :aria-label="$t('_common.close')">
            <x :size="16" class="icon"></x>
          </button>
          <span v-else class="input-group-text">
            <search :size="16" class="icon"></search>
          </span>
        </div>
      </div>
    </div>

    <!-- Apps Grid -->
    <div class="row g-3" v-if="displayedApps.length > 0">
      <div class="col-12 col-sm-6 col-md-4 col-lg-3" v-for="{ app, index } in displayedApps" :key="app.uuid || index"
           :class="{ 'app-dragover': dragOverIndex === index }"
           :draggable="dragEnabled"
           @dragstart="onDragStart($event, index)"
           @dragover="onDragOver($event, index)"
           @dragleave="onDragLeave(index)"
           @drop="onDrop($event, index)"
           @dragend="onDragEnd">
        <div class="card app-card h-100" :class="{ 'border-success': isRunning(app) }">
          <div class="app-poster-container">
            <img
              v-if="app['image-path']"
              :src="coverUrl(app, index)"
              class="app-poster"
              :alt="app.name"
              @error="handleImageError"
            />
            <div v-else class="app-poster-placeholder">
              <span class="app-initial">{{ app.name.charAt(0).toUpperCase() }}</span>
            </div>
            <div class="app-poster-overlay">
              <div v-if="app.cmd" class="app-overlay-row" :title="app.cmd">
                <terminal :size="14" class="icon me-1"></terminal>
                <span class="app-detail-text">{{ app.cmd }}</span>
              </div>
              <div v-if="app['working-dir']" class="app-overlay-row" :title="app['working-dir']">
                <folder :size="14" class="icon me-1"></folder>
                <span class="app-detail-text">{{ app['working-dir'] }}</span>
              </div>
              <div class="app-overlay-badges">
                <span v-if="app.elevated" class="badge app-flag-badge">{{ $t('apps.badge_admin') }}</span>
                <span v-if="app.detached && app.detached.length" class="badge app-flag-badge">{{ $t('apps.badge_detached') }}</span>
                <span v-if="app['prep-cmd'] && app['prep-cmd'].length" class="badge app-flag-badge">{{ $t('apps.badge_app_prep') }}</span>
                <span v-if="app['exclude-global-prep-cmd']" class="badge app-flag-badge">{{ $t('apps.badge_no_global_prep') }}</span>
                <span v-if="app['auto-detach']" class="badge app-flag-badge">{{ $t('apps.badge_auto_detach') }}</span>
              </div>
            </div>
          </div>
          <div class="card-body d-flex flex-column">
            <h5 class="card-title mb-3 d-flex align-items-center gap-2" @dblclick="exportLauncherFile(app)">
              <grip-vertical v-if="dragEnabled" :size="16" class="icon text-muted flex-shrink-0 app-drag-handle"></grip-vertical>
              <span class="flex-grow-1">{{ app.name }}</span>
              <span v-if="isRunning(app)" class="badge bg-success">{{ $t('apps.running') }}</span>
            </h5>
            <div class="mt-auto d-flex gap-2">
              <button class="btn btn-sm btn-primary flex-fill" @click="editApp(index)" :disabled="actionDisabled">
                <edit :size="16" class="icon"></edit>
                {{ $t('apps.edit') }}
              </button>
              <button v-if="isRunning(app)" class="btn btn-sm btn-warning" :disabled="actionDisabled"
                      :title="$t('apps.close')" @click="closeApp">
                <circle-stop :size="16" class="icon"></circle-stop>
              </button>
              <a v-else-if="app.uuid" class="btn btn-sm btn-success" :class="{ disabled: actionDisabled }"
                 :href="launchLink(app)" :title="$t('apps.launch')" @click.prevent="launchApp(app)">
                <play :size="16" class="icon"></play>
              </a>
              <button class="btn btn-sm btn-danger" @click="showDeleteModal(index)" :disabled="actionDisabled">
                <trash-2 :size="16" class="icon"></trash-2>
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- No search results -->
    <div v-else-if="apps && apps.length > 0" class="card">
      <div class="card-body text-center py-5">
        <p class="text-muted">{{ $t('apps.no_search_results') }}</p>
      </div>
    </div>

    <!-- Empty State -->
    <div v-else class="card">
      <div class="card-body text-center py-5">
        <p class="text-muted">{{ $t('apps.no_applications') }}</p>
      </div>
    </div>

    <!-- Edit / Add Application modal -->
    <div class="modal fade" ref="editModal" tabindex="-1" aria-labelledby="appEditModalLabel"
         aria-hidden="true" data-bs-backdrop="static" data-bs-keyboard="false">
      <div class="modal-dialog modal-xl modal-dialog-scrollable modal-fullscreen-lg-down">
        <div class="modal-content" v-if="editForm">
          <div class="modal-header">
            <h5 class="modal-title" id="appEditModalLabel">
              {{ editModalTitle }}
            </h5>
            <button type="button" class="btn-close" @click="closeEditModal"
              :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <!-- Error -->
            <div v-if="editFormError" class="alert alert-danger py-2 small">{{ editFormError }}</div>
            <div v-if="actionError" class="alert alert-danger py-2 small">{{ actionError }}</div>
            <!-- Application Name -->
            <div class="mb-3">
              <label for="appName" class="form-label">{{ $t('apps.app_name') }}</label>
              <input type="text" class="form-control" id="appName" aria-describedby="appNameHelp" v-model="editForm.name" />
              <div id="appNameHelp" class="form-text">{{ $t('apps.app_name_desc') }}</div>
            </div>
            <!-- gamepad override -->
            <div class="mb-3">
              <label for="appGamepad" class="form-label">{{ $t('config.gamepad') }}</label>
              <select id="appGamepad" class="form-select" v-model="editForm.gamepad">
                <option value="">{{ $t('_common.default_global') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="auto">{{ $t('_common.auto') }}</option>
                <option value="generic">{{ $t('config.gamepad_generic') }}</option>
                <option value="x360">{{ $t('config.gamepad_x360') }}</option>
                <option value="xone">{{ $t('config.gamepad_xone') }}</option>
                <option value="xseries">{{ $t('config.gamepad_xseries') }}</option>
                <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
                <option value="ds5">{{ $t('config.gamepad_ds5') }}</option>
                <option value="switch">{{ $t('config.gamepad_switch') }}</option>
              </select>
              <div class="form-text">{{ $t('apps.gamepad_desc') }}</div>
            </div>
            <!-- output -->
            <div class="mb-3">
              <label for="appOutput" class="form-label">{{ $t('apps.output_name') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appOutput" aria-describedby="appOutputHelp"
                  v-model="editForm.output" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('any', 'file_browser.select_file', editForm.output, v => editForm.output = v)">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
              </div>
              <div id="appOutputHelp" class="form-text">{{ $t('apps.output_desc') }}</div>
            </div>
            <!-- allow client commands -->
            <Checkbox class="mb-3"
                      id="clientCommands"
                      label="apps.allow_client_commands"
                      desc="apps.allow_client_commands_desc"
                      v-model="editForm['allow-client-commands']"
                      default="true"
            ></Checkbox>
            <!-- prep-cmd and state-cmd -->
            <template v-for="type in ['prep', 'state']" :key="type">
              <Checkbox class="mb-3"
                        :id="`excludeGlobal_${type}`"
                        :label="`apps.global_${type}_name`"
                        :desc="`apps.global_${type}_desc`"
                        v-model="editForm[`exclude-global-${type}-cmd`]"
                        default="true"
                        inverse-values
              ></Checkbox>
              <div class="mb-3">
                <div class="form-label">{{ $t(`apps.cmd_${type}_name`) }}</div>
                <div class="form-text pre-wrap">{{ $t(`apps.cmd_${type}_desc`) }}</div>
                <div class="d-flex justify-content-start mb-3 mt-3" v-if="editForm[`${type}-cmd`].length === 0">
                  <button class="btn btn-success" @click="addCmd(type)">
                    <plus :size="18" class="icon"></plus>
                    {{ $t('apps.add_cmds') }}
                  </button>
                </div>
                <table class="table" v-if="editForm[`${type}-cmd`].length > 0">
                  <thead>
                    <tr>
                      <th scope="col">
                        <play :size="18" class="icon"></play>
                        {{ $t('_common.do_cmd') }}
                      </th>
                      <th scope="col">
                        <rotate-ccw :size="18" class="icon"></rotate-ccw>
                        {{ $t('_common.undo_cmd') }}
                      </th>
                      <th scope="col" v-if="canElevate">
                        <shield :size="18" class="icon"></shield>
                        {{ $t('_common.run_as') }}
                      </th>
                      <th scope="col"></th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-for="(c, i) in editForm[`${type}-cmd`]" :key="i">
                      <td>
                        <div class="input-group">
                          <label :for="`${type}-cmd-do-${i}`" class="visually-hidden">{{ $t('_common.do_cmd') }}</label>
                          <input :id="`${type}-cmd-do-${i}`" type="text" class="form-control monospace" v-model="c.do" />
                          <button class="btn btn-secondary btn-sm" type="button" @click="browseCmd(type, i, 'do')">
                            <folder-open :size="14" class="icon"></folder-open>
                          </button>
                        </div>
                      </td>
                      <td>
                        <div class="input-group">
                          <label :for="`${type}-cmd-undo-${i}`" class="visually-hidden">{{ $t('_common.undo_cmd') }}</label>
                          <input :id="`${type}-cmd-undo-${i}`" type="text" class="form-control monospace" v-model="c.undo" />
                          <button class="btn btn-secondary btn-sm" type="button" @click="browseCmd(type, i, 'undo')">
                            <folder-open :size="14" class="icon"></folder-open>
                          </button>
                        </div>
                      </td>
                      <td v-if="canElevate" class="align-middle">
                        <Checkbox :id="`${type}-cmd-admin-${i}`"
                                  label="_common.elevated"
                                  desc=""
                                  v-model="c.elevated"
                        ></Checkbox>
                      </td>
                      <td class="align-middle text-nowrap">
                        <button class="btn btn-danger btn-sm ms-2" @click="deleteCmd(type, i)">
                          <trash-2 :size="16" class="icon"></trash-2>
                        </button>
                        <button class="btn btn-success btn-sm ms-2" @click="addCmd(type, i + 1)">
                          <plus :size="16" class="icon"></plus>
                        </button>
                      </td>
                    </tr>
                  </tbody>
                </table>
              </div>
            </template>
            <!-- detached -->
            <div class="mb-3">
              <label for="appName" class="form-label">{{ $t('apps.detached_cmds') }}</label>
              <div v-for="(c,i) in editForm.detached" :key="i" class="d-flex justify-content-between align-items-center my-2">
                <label :for="`detached-command-${i}`" class="visually-hidden">{{ $t('apps.detached_cmds') }}</label>
                <input :id="`detached-command-${i}`" type="text" v-model="editForm.detached[i]" class="form-control monospace">
                <button class="btn btn-secondary btn-sm ms-2" @click="browseDetached(i)">
                  <folder-open :size="14" class="icon"></folder-open>
                </button>
                <button class="btn btn-danger btn-sm ms-2" @click="editForm.detached.splice(i,1)">
                  <trash-2 :size="16" class="icon"></trash-2>
                </button>
                <button class="btn btn-success btn-sm ms-2" @click="addDetached">
                  <plus :size="16" class="icon"></plus>
                </button>
              </div>
              <div class="d-flex justify-content-start mb-3 mt-3" v-if="editForm.detached.length === 0">
                <button class="btn btn-success" @click="addDetached">
                  <plus :size="18" class="icon"></plus>
                  {{ $t('apps.detached_cmds_add') }}
                </button>
              </div>
              <div class="form-text">
                {{ $t('apps.detached_cmds_desc') }}<br>
                <b>{{ $t('_common.note') }}</b> {{ $t('apps.detached_cmds_note') }}
              </div>
            </div>
            <!-- command -->
            <div class="mb-3">
              <label for="appCmd" class="form-label">{{ $t('apps.cmd') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appCmd" aria-describedby="appCmdHelp"
                  v-model="editForm.cmd" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('executable', 'file_browser.select_executable', editForm.cmd, v => editForm.cmd = v)">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
              </div>
              <div id="appCmdHelp" class="form-text">
                {{ $t('apps.cmd_desc') }}<br>
                <b>{{ $t('_common.note') }}</b> {{ $t('apps.cmd_note') }}
              </div>
            </div>
            <!-- working dir -->
            <div class="mb-3">
              <label for="appWorkingDir" class="form-label">{{ $t('apps.working_dir') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appWorkingDir" aria-describedby="appWorkingDirHelp"
                  v-model="editForm['working-dir']" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('directory', 'file_browser.select_directory', editForm['working-dir'], v => editForm['working-dir'] = v)">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
              </div>
              <div id="appWorkingDirHelp" class="form-text">{{ $t('apps.working_dir_desc') }}</div>
            </div>
            <!-- elevation -->
            <Checkbox v-if="canElevate"
                      class="mb-3"
                      id="appElevation"
                      label="_common.run_as"
                      desc="apps.run_as_desc"
                      v-model="editForm.elevated"
                      default="false"
            ></Checkbox>
            <!-- auto-detach -->
            <Checkbox class="mb-3"
                      id="autoDetach"
                      label="apps.auto_detach"
                      desc="apps.auto_detach_desc"
                      v-model="editForm['auto-detach']"
                      default="true"
            ></Checkbox>
            <!-- wait for all processes -->
            <Checkbox class="mb-3"
                      id="waitAll"
                      label="apps.wait_all"
                      desc="apps.wait_all_desc"
                      v-model="editForm['wait-all']"
                      default="true"
            ></Checkbox>
            <!-- terminate on pause -->
            <Checkbox class="mb-3"
                      id="terminateOnPause"
                      label="apps.terminate_on_pause"
                      desc="apps.terminate_on_pause_desc"
                      v-model="editForm['terminate-on-pause']"
                      default="false"
            ></Checkbox>
            <!-- exit timeout -->
            <div class="mb-3">
              <label for="exitTimeout" class="form-label">{{ $t('apps.exit_timeout') }}</label>
              <input type="number" class="form-control monospace" id="exitTimeout" aria-describedby="exitTimeoutHelp"
                     v-model="editForm['exit-timeout']" min="0" placeholder="5" />
              <div id="exitTimeoutHelp" class="form-text">{{ $t('apps.exit_timeout_desc') }}</div>
            </div>
            <div class="mb-3">
              <label for="appImagePath" class="form-label">{{ $t('apps.image') }}</label>
              <div class="input-group">
                <input type="text" class="form-control monospace" id="appImagePath" aria-describedby="appImagePathHelp"
                  v-model="editForm['image-path']" />
                <button class="btn btn-secondary" type="button"
                  @click="browseFor('file', 'file_browser.select_file', editForm['image-path'], v => editForm['image-path'] = v, ['.png'])">
                  <folder-open :size="18" class="icon"></folder-open>
                </button>
                <button class="btn btn-secondary" type="button" @click="showCoverFinder">
                  <search :size="18" class="icon"></search>
                  {{ $t('apps.find_cover') }}
                </button>
              </div>
              <div id="appImagePathHelp" class="form-text">{{ $t('apps.image_desc') }}</div>
            </div>
            <!-- use virtual display -->
            <Checkbox class="mb-3"
                      id="virtualDisplay"
                      label="apps.virtual_display"
                      :desc="platform === 'linux' ? 'apps.virtual_display_desc_linux' : 'apps.virtual_display_desc'"
                      v-model="editForm['virtual-display']"
                      default="false"
            ></Checkbox>
            <!-- use app identity -->
            <Checkbox class="mb-3"
                      id="useAppIdentity"
                      label="apps.use_app_identity"
                      desc="apps.use_app_identity_desc"
                      v-model="editForm['use-app-identity']"
                      default="false"
            ></Checkbox>
            <!-- per-client app identity -->
            <Checkbox v-if="editForm['use-app-identity']"
                      class="mb-3"
                      id="perClientAppIdentity"
                      label="apps.per_client_app_identity"
                      desc="apps.per_client_app_identity_desc"
                      v-model="editForm['per-client-app-identity']"
                      default="false"
            ></Checkbox>
            <!-- resolution scale factor -->
            <div class="mb-3" v-if="platform === 'windows' || platform === 'linux'">
              <label for="resolutionScaleFactor" class="form-label">{{ $t('apps.resolution_scale_factor') }}: {{ editForm['scale-factor'] }}%</label>
              <input type="range" step="1" min="20" max="200" class="form-range" id="resolutionScaleFactor" v-model.number="editForm['scale-factor']" />
              <div class="form-text">{{ $t('apps.resolution_scale_factor_desc') }}</div>
            </div>
            <div class="env-hint alert alert-info">
              <div class="form-text">
                <h4>{{ $t('apps.env_vars_about') }}</h4>
                {{ $t('apps.env_vars_desc') }}
              </div>
              <table class="env-table">
                <thead>
                  <tr>
                    <th scope="col">{{ $t('apps.env_var_name') }}</th>
                    <th scope="col">{{ $t('apps.env_var_description') }}</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="envVar in envVars" :key="envVar.name">
                    <td style="font-family: monospace">{{ envVar.name }}</td>
                    <td>{{ $t(envVar.desc) }}</td>
                  </tr>
                </tbody>
              </table>
              <div class="form-text" v-if="platform === 'windows'"><b>{{ $t('apps.env_rtss_cli_example') }}</b>
                <pre>cmd /C \path\to\rtss-cli.exe limit:set %APOLLO_CLIENT_FPS%</pre>
              </div>
              <div class="form-text" v-else-if="platform === 'freebsd' || platform === 'linux'"><b>{{ $t('apps.env_xrandr_example') }}</b>
                <pre>sh -c "xrandr --output HDMI-1 --mode \"${APOLLO_CLIENT_WIDTH}x${APOLLO_CLIENT_HEIGHT}\" --rate ${APOLLO_CLIENT_FPS}"</pre>
              </div>
              <div class="form-text" v-else-if="platform === 'macos'"><b>{{ $t('apps.env_displayplacer_example') }}</b>
                <pre>sh -c "displayplacer "id:&lt;screenId&gt; res:${APOLLO_CLIENT_WIDTH}x${APOLLO_CLIENT_HEIGHT} hz:${APOLLO_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre>
              </div>
              <div class="form-text"><a
                  :href="`${documentationBaseUrl}/md_docs_2app__examples.html`"
                  target="_blank">{{ $t('_common.see_more') }}</a></div>
            </div>
            <div class="alert alert-info">
              <info :size="18" class="icon"></info>
              {{ $t('apps.env_sunshine_compatibility') }}
            </div>
          </div>
          <!-- Save buttons -->
          <div class="modal-footer">
            <button class="btn btn-success me-auto" @click="exportLauncherFile(editForm)" :disabled="!editForm.uuid">
              <download :size="18" class="icon"></download>
              {{ $t('apps.export_launcher_file') }}
            </button>
            <button @click="closeEditModal" class="btn btn-secondary">
              <x :size="18" class="icon"></x>
              {{ $t('_common.cancel') }}
            </button>
            <button class="btn btn-primary" @click="save" :disabled="actionDisabled || !(editForm.name || '').trim()">
              <save :size="18" class="icon"></save>
              {{ $t('_common.save') }}
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- Cover Finder modal -->
    <div class="modal fade" id="coverFinderModal" tabindex="-1" aria-labelledby="coverFinderModalLabel" aria-hidden="true" ref="coverFinderModal">
      <div class="modal-dialog modal-xl modal-dialog-scrollable modal-fullscreen-md-down">
        <div class="modal-content">
          <div class="modal-header">
            <h5 class="modal-title" id="coverFinderModalLabel">
              <span v-if="coverSearching">{{ $t('apps.searching_covers') }}</span>
              <span v-else-if="coverCandidates.length > 0">{{ $t('apps.covers_found') }} ({{ coverCandidates.length }})</span>
              <span v-else>{{ $t('apps.no_covers_found') }}</span>
            </h5>
            <button type="button" class="btn-close" data-bs-dismiss="modal" :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <div class="mb-3">
              <div class="input-group">
                <label for="cover-search-query" class="visually-hidden">{{ $t('_common.search') }}</label>
                <input
                  id="cover-search-query"
                  type="text"
                  class="form-control"
                  v-model="coverSearchQuery"
                  :placeholder="editForm?.name"
                  @keyup.enter="performCoverSearch"
                />
                <button class="btn btn-primary" type="button" @click="performCoverSearch">
                  <search :size="18" class="icon"></search>
                  {{ $t('_common.search') }}
                </button>
              </div>
              <div class="form-text mt-2">
                <b>{{ $t('_common.note') }}</b> {{ $t('apps.cover_search_hint') }}
                <a href="https://www.igdb.com/" target="_blank" rel="noopener noreferrer">IGDB</a>
              </div>
            </div>
            <div class="cover-results" :class="{ busy: coverFinderBusy }">
              <div class="row">
                <div v-if="coverSearching" class="col-12 col-sm-6 col-lg-4 mb-3">
                  <div class="cover-container">
                    <output class="spinner-border">
                      <span class="visually-hidden">{{ $t('apps.loading') }}</span>
                    </output>
                  </div>
                </div>
                <button type="button" v-for="cover in coverCandidates" :key="cover.url" class="cover-choice col-12 col-sm-6 col-lg-3 mb-3"
                  @click="useCover(cover)">
                  <div class="cover-container result">
                    <img class="rounded" :src="cover.url" :alt="cover.name" />
                  </div>
                  <span class="d-block text-nowrap text-center text-truncate">
                    {{cover.name}}
                  </span>
                </button>
              </div>
            </div>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">
              <x :size="18" class="icon"></x>
              {{ $t('_common.cancel') }}
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- Delete confirmation modal -->
    <div class="modal fade" ref="deleteModal" tabindex="-1" aria-labelledby="appDeleteModalLabel" aria-hidden="true">
      <div class="modal-dialog modal-dialog-centered">
        <div class="modal-content" v-if="deleteTarget">
          <div class="modal-header">
            <h5 class="modal-title" id="appDeleteModalLabel">{{ $t('apps.delete_title') }}</h5>
            <button type="button" class="btn-close" @click="closeDeleteModal"
              :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <i18n-t keypath="apps.delete_confirm" tag="span">
              <template #name><strong>{{ deleteTarget.name }}</strong></template>
            </i18n-t>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-secondary" @click="closeDeleteModal">
              <x :size="18" class="icon"></x>
              {{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-danger" @click="confirmDelete">
              <trash-2 :size="18" class="icon"></trash-2>
              {{ $t('apps.delete') }}
            </button>
          </div>
        </div>
      </div>
    </div>

    <!-- Shared file browser modal -->
    <div class="modal fade" ref="fileBrowserModal" tabindex="-1" aria-hidden="true">
      <div class="modal-dialog modal-lg modal-dialog-scrollable modal-fullscreen-md-down">
        <div class="modal-content">
          <div class="modal-header">
            <h5 class="modal-title">{{ fileBrowserTitle || $t('file_browser.title') }}</h5>
            <button type="button" class="btn-close" @click="fileBrowserClose" :aria-label="$t('_common.close')"></button>
          </div>
          <div class="modal-body">
            <!-- Path input -->
            <div class="input-group mb-2">
              <label for="file-browser-path" class="visually-hidden">{{ $t('file_browser.title') }}</label>
              <input id="file-browser-path" type="text" class="form-control monospace" v-model="fileBrowserTypedPath"
                @input="fileBrowserOnTypedInput" @keyup.enter="fileBrowserNavigate(fileBrowserTypedPath)" />
              <button class="btn btn-secondary" type="button" @click="fileBrowserNavigate(fileBrowserTypedPath)">
                <arrow-right :size="16" class="icon"></arrow-right>
              </button>
            </div>
            <!-- Up button -->
            <div class="mb-2">
              <button class="btn btn-sm btn-outline-secondary" type="button"
                :disabled="fileBrowserLoading || fileBrowserParentPath === fileBrowserCurrentPath"
                @click="fileBrowserNavigateUp">
                <folder-up :size="16" class="icon me-1"></folder-up>
                {{ $t('file_browser.up') }}
              </button>
            </div>
            <!-- Error -->
            <div v-if="fileBrowserError" class="alert alert-danger py-2 small">{{ fileBrowserError }}</div>
            <!-- Loading -->
            <div v-if="fileBrowserLoading" class="text-center py-3">
              <output class="spinner-border spinner-border-sm">
                <span class="visually-hidden">{{ $t('_common.loading') }}</span>
              </output>
            </div>
            <!-- Entries -->
            <div v-else class="list-group" style="max-height: 400px; overflow-y: auto;">
              <div v-if="fileBrowserEntries.length === 0" class="list-group-item text-muted text-center">
                {{ $t('file_browser.empty') }}
              </div>
              <button v-for="entry in fileBrowserEntries" :key="entry.path" type="button"
                class="list-group-item list-group-item-action d-flex align-items-center py-1"
                :class="{ active: fileBrowserSelectedPath === entry.path }"
                @click="fileBrowserSelectEntry(entry)" @dblclick="fileBrowserActivateEntry(entry)">
                <hard-drive v-if="!fileBrowserCurrentPath && entry.type === 'directory'" :size="16" class="icon me-2 flex-shrink-0"></hard-drive>
                <folder v-else-if="entry.type === 'directory'" :size="16" class="icon me-2 flex-shrink-0 text-warning"></folder>
                <file-text v-else :size="16" class="icon me-2 flex-shrink-0"></file-text>
                <span class="text-truncate">{{ entry.name }}</span>
              </button>
            </div>
          </div>
          <div class="modal-footer flex-wrap gap-2">
            <div class="flex-grow-1 text-muted small text-truncate" v-if="fileBrowserSelectedPath">
              <code>{{ fileBrowserSelectedPath }}</code>
            </div>
            <button type="button" class="btn btn-secondary" @click="fileBrowserClose">
              <x :size="16" class="icon me-1"></x>
              {{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-primary" @click="fileBrowserConfirm"
              :disabled="!fileBrowserSelectedPath && !fileBrowserTypedPath">
              <check :size="16" class="icon me-1"></check>
              {{ $t('file_browser.select') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
  import { toRaw } from 'vue'
  import Navbar from './Navbar.vue'
  import Checkbox from './Checkbox.vue'
  import { apiFetch } from './fetch_utils'
  import { SUNSHINE_DOCS_URL } from './links'
  import { Modal } from 'bootstrap/dist/js/bootstrap'
  import {
    ArrowDown,
    ArrowRight,
    ArrowUp,
    ArrowUpDown,
    Check,
    CircleStop,
    Download,
    Edit,
    FileText,
    Folder,
    FolderOpen,
    FolderUp,
    GripVertical,
    HardDrive,
    Info,
    LayersPlus,
    Play,
    Plus,
    RotateCcw,
    Save,
    Search,
    Shield,
    Terminal,
    Trash2,
    X,
  } from '@lucide/vue'

  /**
   * Values of the optional application fields that the editor shows when an application does not set them.
   * They match the defaults the server applies in `proc::parse()`.
   */
  export const APP_FIELD_DEFAULTS = Object.freeze({
    "prep-cmd": [],
    "state-cmd": [],
    detached: [],
    "exclude-global-prep-cmd": false,
    "exclude-global-state-cmd": false,
    elevated: false,
    "auto-detach": true,
    "wait-all": true,
    "exit-timeout": 5,
    cmd: "",
    output: "",
    "working-dir": "",
    "image-path": "",
    gamepad: "",
    "allow-client-commands": true,
    "terminate-on-pause": false,
    "virtual-display": false,
    "use-app-identity": false,
    "per-client-app-identity": false,
    "scale-factor": 100,
  });

  /** Environment variables Apollo sets for application commands. */
  const ENV_VARS = [
    ["APOLLO_APP_ID", "apps.env_app_id"],
    ["APOLLO_APP_NAME", "apps.env_app_name"],
    ["APOLLO_APP_UUID", "apps.env_app_uuid"],
    ["APOLLO_APP_STATUS", "apps.env_app_status"],
    ["APOLLO_CLIENT_UUID", "apps.env_client_uuid"],
    ["APOLLO_CLIENT_NAME", "apps.env_client_name"],
    ["APOLLO_CLIENT_WIDTH", "apps.env_client_width"],
    ["APOLLO_CLIENT_HEIGHT", "apps.env_client_height"],
    ["APOLLO_CLIENT_FPS", "apps.env_client_fps"],
    ["APOLLO_CLIENT_HDR", "apps.env_client_hdr"],
    ["APOLLO_CLIENT_GCMAP", "apps.env_client_gcmap"],
    ["APOLLO_CLIENT_HOST_AUDIO", "apps.env_client_host_audio"],
    ["APOLLO_CLIENT_ENABLE_SOPS", "apps.env_client_enable_sops"],
    ["APOLLO_CLIENT_AUDIO_CONFIGURATION", "apps.env_client_audio_config"],
  ].map(([name, desc]) => ({ name, desc }));

  /**
   * Build the `.art` launcher file Artemis imports to start an application directly.
   *
   * @param {{uuid: string, name: string}} app Application.
   * @param {string} hostUUID Host unique id.
   * @param {string} hostName Host name.
   * @returns {string} File contents.
   */
  export function launcherFileContent(app, hostUUID, hostName) {
    return `# Artemis app entry
# Exported by Apollo
# https://github.com/ClassicOldSong/Apollo

[host_uuid] ${hostUUID}
[host_name] ${hostName}
[app_uuid] ${app.uuid}
[app_name] ${app.name}
`;
  }

  /**
   * Return the GameDB bucket for an application name.
   *
   * @param {string} name Application name to categorize.
   * @returns {string} The normalized GameDB bucket name.
   */
  function getSearchBucket(name) {
    const bucket = name.substring(0, Math.min(name.length, 2)).toLowerCase().replaceAll(/[^a-z\d]/g, '');
    return bucket || '@';
  }

  /**
   * Search GameDB for cover candidates matching an application name.
   *
   * @param {string} name Application name to search for.
   * @returns {Promise<object[]>} Matching cover candidates.
   */
  function searchCovers(name) {
    if (!name) {
      return Promise.resolve([]);
    }
    let searchName = name.replaceAll(/\s+/g, '.').toLowerCase();

    // Use raw.githubusercontent.com to avoid CORS issues as we migrate the CNAME
    let dbUrl = "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages";
    let bucket = getSearchBucket(name);
    return fetch(`${dbUrl}/buckets/${bucket}.json`).then(function (r) {
      if (!r.ok) throw new Error("Failed to search covers");
      return r.json();
    }).then(maps => Promise.all(Object.keys(maps).map(id => {
      let item = maps[id];
      if (item.name.replaceAll(/\s+/g, '.').toLowerCase().startsWith(searchName)) {
        return fetch(`${dbUrl}/games/${id}.json`).then(function (r) {
          return r.json();
        }).catch(() => null);
      }
      return null;
    }).filter(Boolean)))
      .then(results => results
        .filter(item => item && item.cover && item.cover.url)
        .map(game => {
          const thumb = game.cover.url;
          const dotIndex = thumb.lastIndexOf('.');
          const slashIndex = thumb.lastIndexOf('/');
          if (dotIndex < 0 || slashIndex < 0) {
            return null;
          }
          const slug = thumb.substring(slashIndex + 1, dotIndex);
          return {
            name: game.name,
            key: `igdb_${game.id}`,
            url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
            saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
          }
        }).filter(Boolean));
  }

  export default {
    components: {
      Navbar,
      Checkbox,
      ArrowDown,
      ArrowRight,
      ArrowUp,
      ArrowUpDown,
      Check,
      CircleStop,
      Download,
      Edit,
      FileText,
      Folder,
      FolderOpen,
      FolderUp,
      GripVertical,
      HardDrive,
      Info,
      LayersPlus,
      Play,
      Plus,
      RotateCcw,
      Save,
      Search,
      Shield,
      Terminal,
      Trash2,
      X,
    },
    data() {
      return {
        apps: [],
        editForm: null,
        editFormError: "",
        detachedCmd: "",
        coverSearching: false,
        coverFinderBusy: false,
        coverCandidates: [],
        coverSearchQuery: "",
        platform: "",
        fileBrowserType: "any",
        fileBrowserTitle: "",
        fileBrowserCallback: null,
        fileBrowserAcceptedExtensions: null,
        fileBrowserCurrentPath: "",
        fileBrowserParentPath: "",
        fileBrowserEntries: [],
        fileBrowserLoading: false,
        fileBrowserError: "",
        fileBrowserSelectedPath: "",
        fileBrowserTypedPath: "",
        searchQuery: "",
        sortMode: "default",
        deleteTarget: null,
        // Apollo: running app, host identity for launch links, and reordering.
        actionDisabled: false,
        actionError: "",
        currentApp: "",
        hostName: "",
        hostUUID: "",
        dragIndex: -1,
        dragOverIndex: -1,
        orderChanged: false,
        // Position of each application (by UUID) in the saved list; covers are served by that position.
        savedPositions: new Map(),
        envVars: ENV_VARS,
      };
    },
    computed: {
      documentationBaseUrl() {
        return SUNSHINE_DOCS_URL
      },
      canElevate() {
        return this.platform === 'windows' || this.platform === 'linux';
      },
      /** Dragging changes the saved order, so it is only offered on the unfiltered, unsorted list. */
      dragEnabled() {
        return this.sortMode === "default" && !this.searchQuery.trim() && !this.actionDisabled;
      },
      /** A dragged order, or the name-sorted view of the whole list, can be saved as the app order. */
      canSaveOrder() {
        return !this.searchQuery.trim() && (this.orderChanged || (this.sortMode !== "default" && this.apps.length > 1));
      },
      displayedApps() {
        let list = this.apps.map((app, index) => ({ app, index }));

        const query = this.searchQuery.trim().toLowerCase();
        if (query) {
          list = list.filter(({ app }) =>
            (app.name || "").toLowerCase().includes(query)
          );
        }

        if (this.sortMode !== "default") {
          // Apps can be created without a name, so we compare name || ""
          list.sort((a, b) => {
            const result = (a.app.name || "").localeCompare(
              b.app.name || "", undefined, { sensitivity: "base" }
            );
            // localeCompare returns 0 if the strings are equal, a negative value if a < b, and a positive value if a > b
            return this.sortMode === "asc"
              ? result
              : -result;
          });
        }

        return list;
      },
      sortModeLabel() {
        switch (this.sortMode) {
          case "asc":
            return this.$t("apps.sort_ascending");
          case "desc":
            return this.$t("apps.sort_descending");
          default:
            return this.$t("apps.sort_default");
        }
      },
      appCountLabel() {
        const total = this.apps.length;
        const shown = this.displayedApps.length;
        return shown === total
          ? `${total}`
          : `${shown} / ${total}`;
      },
      editModalTitle() {
        if (!this.editForm) {
          return "";
        }
        const action = this.editForm.index === -1
          ? this.$t("apps.add_new")
          : this.$t("apps.edit");

        return this.editForm.name
          ? `${action}: ${this.editForm.name}`
          : action;
      },
    },
    created() {
      this.loadApps();

      apiFetch("./api/config")
        .then(r => r.json())
        .then(r => {
          this.platform = r.platform;
        })
        .catch((e) => console.error(e));
    },
    methods: {
      newApp() {
        this.editForm = {
          name: "",
          index: -1,
          ...structuredClone(APP_FIELD_DEFAULTS),
        };
        this.editFormError = "";
        this.actionError = "";
        this.openEditModal();
      },
      /**
       * @brief Open an application for editing with nullish optional fields defaulted.
       * @param {number} id Index of the application to edit.
       */
      editApp(id) {
        this.editForm = structuredClone(toRaw(this.apps[id]));
        this.editForm.index = id;
        for (const [key, value] of Object.entries(APP_FIELD_DEFAULTS)) {
          this.editForm[key] ??= structuredClone(value);
        }
        // The elevation Checkbox emits values of its model's type: a missing flag would toggle to the string
        // "true", which the server's command reader rejects. Give it a boolean.
        if (this.canElevate) {
          for (const command of [...this.editForm["prep-cmd"], ...this.editForm["state-cmd"]]) {
            command.elevated = command.elevated === true || command.elevated === "true";
          }
        }
        this.editFormError = "";
        this.actionError = "";
        this.openEditModal();
      },
      showDeleteModal(id) {
        this.deleteTarget = { index: id, uuid: this.apps[id].uuid, name: this.apps[id].name };
        this.$nextTick(() => {
          Modal.getOrCreateInstance(this.$refs.deleteModal).show();
        });
      },
      closeDeleteModal() {
        const modal = Modal.getInstance(this.$refs.deleteModal);
        if (modal) modal.hide();
      },
      /**
       * @brief Load the applications, the running application and the host identity.
       *
       * @return {Promise<void>} Completion of the request.
       */
      loadApps() {
        return apiFetch("./api/apps")
          .then((r) => r.json())
          .then((r) => {
            this.apps = r.apps ?? [];
            this.savedPositions = new Map(this.apps.map((app, i) => [app.uuid, i]).filter(([uuid]) => uuid));
            this.currentApp = r.current_app ?? "";
            this.hostName = r.host_name ?? "";
            this.hostUUID = r.host_uuid ?? "";
            this.orderChanged = false;
          });
      },
      /**
       * @brief Delete the application chosen in the delete dialog: by UUID, or by position for an application
       * saved before UUIDs existed.
       *
       * @return {Promise<void>} Completion of the request.
       */
      async confirmDelete() {
        const target = this.deleteTarget;
        const request = target.uuid
          ? apiFetch("./api/apps/delete", {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            },
            body: JSON.stringify({ uuid: target.uuid }),
          })
          : apiFetch("./api/apps/" + target.index, {
            method: "DELETE",
            headers: {
              "Content-Type": "application/json"
            },
          });
        const r = await request;
        this.closeDeleteModal();
        if (r.status === 200) {
          await this.loadApps();
        } else {
          this.actionError = this.$t('apps.delete_failed') + (await this.errorText(r));
        }
      },
      /**
       * @brief Read the error message of a failed API response.
       *
       * @param {Response} response Failed response.
       * @return {Promise<string>} Error message.
       */
      async errorText(response) {
        try {
          const body = await response.json();
          return body.error || body.message || `HTTP ${response.status}`;
        } catch (e) {
          return `HTTP ${response.status}`;
        }
      },
      /**
       * @brief Insert an empty preparation or state command.
       *
       * @param {string} type `prep` or `state`.
       * @param {number} [index] Insert position; appended when omitted.
       */
      addCmd(type, index) {
        const commands = this.editForm[`${type}-cmd`];
        const template = { do: "", undo: "" };
        if (this.canElevate) {
          template.elevated = false;
        }
        commands.splice(index ?? commands.length, 0, template);
      },
      deleteCmd(type, index) {
        this.editForm[`${type}-cmd`].splice(index, 1);
      },
      addPrepCmd() {
        this.addCmd("prep");
      },
      deletePrepCmd(index) {
        this.deleteCmd("prep", index);
      },
      /**
       * @brief Cover image URL. `/api/covers/<n>` takes the position in the saved list, which differs from the
       * shown position while a dragged order is not saved yet.
       *
       * @param {object} app Application.
       * @param {number} index Position in the shown list.
       * @return {string} Cover URL.
       */
      coverUrl(app, index) {
        const position = app.uuid ? this.savedPositions.get(app.uuid) : undefined;
        return `/api/covers/${position ?? index}`;
      },
      isRunning(app) {
        return !!app.uuid && app.uuid === this.currentApp;
      },
      /**
       * @brief Artemis deep link that launches the application from a client.
       *
       * @param {{uuid: string, name: string}} app Application.
       * @return {string} `art://launch` URL.
       */
      launchLink(app) {
        const params = new URLSearchParams({
          host_uuid: this.hostUUID,
          host_name: this.hostName,
          app_uuid: app.uuid,
          app_name: app.name,
        });
        return `art://launch?${params.toString()}`;
      },
      /**
       * @brief Launch an application: through a local Artemis client when the page is opened remotely,
       * otherwise on the host (`POST /api/apps/launch`).
       *
       * @param {object} app Application to launch.
       * @return {Promise<void>} Completion of the launch.
       */
      async launchApp(app) {
        const isLocalHost = ['localhost', '127.0.0.1', '[::1]'].includes(globalThis.location.hostname);
        if (!isLocalHost && globalThis.confirm(this.$t('apps.launch_local_client'))) {
          globalThis.location.assign(this.launchLink(app));
          return;
        }
        if (!globalThis.confirm(this.$t('apps.launch_warning'))) {
          return;
        }
        this.actionDisabled = true;
        this.actionError = "";
        try {
          const r = await apiFetch("./api/apps/launch", {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            },
            body: JSON.stringify({ uuid: app.uuid }),
          });
          if (r.status !== 200) {
            this.actionError = this.$t('apps.launch_failed') + (await this.errorText(r));
          }
        } finally {
          this.actionDisabled = false;
          await this.loadApps();
        }
      },
      /**
       * @brief Terminate the running application after confirmation.
       *
       * @return {Promise<void>} Completion of the request.
       */
      async closeApp() {
        if (!globalThis.confirm(this.$t('apps.close_warning'))) {
          return;
        }
        this.actionDisabled = true;
        this.actionError = "";
        try {
          const r = await apiFetch("./api/apps/close", {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            },
          });
          if (r.status !== 200) {
            this.actionError = this.$t('apps.close_failed');
          }
        } finally {
          this.actionDisabled = false;
          await this.loadApps();
        }
      },
      /**
       * @brief Download the `.art` launcher file of an application.
       *
       * @param {object} app Application with a UUID.
       */
      exportLauncherFile(app) {
        if (!app.uuid) {
          return;
        }
        const link = document.createElement('a');
        link.href = `data:text/plain;charset=utf-8,${encodeURIComponent(launcherFileContent(app, this.hostUUID, this.hostName))}`;
        link.download = `${app.name}.art`;
        link.click();
      },
      onDragStart(event, index) {
        if (!this.dragEnabled) {
          event.preventDefault();
          return;
        }
        this.dragIndex = index;
        event.dataTransfer.effectAllowed = "move";
      },
      onDragOver(event, index) {
        if (this.dragIndex < 0) {
          return;
        }
        event.preventDefault();
        event.dataTransfer.dropEffect = "move";
        this.dragOverIndex = index;
      },
      onDragLeave(index) {
        if (this.dragOverIndex === index) {
          this.dragOverIndex = -1;
        }
      },
      onDrop(event, index) {
        if (this.dragIndex < 0) {
          return;
        }
        event.preventDefault();
        this.moveApp(this.dragIndex, index);
        this.onDragEnd();
      },
      onDragEnd() {
        this.dragIndex = -1;
        this.dragOverIndex = -1;
      },
      /**
       * @brief Move an application to another position in the list (not saved until "Save Order").
       *
       * @param {number} from Current index.
       * @param {number} to Index of the application it is dropped on.
       */
      moveApp(from, to) {
        if (from === to || from < 0 || to < 0 || from >= this.apps.length || to >= this.apps.length) {
          return;
        }
        const [moved] = this.apps.splice(from, 1);
        this.apps.splice(to, 0, moved);
        this.orderChanged = true;
      },
      /**
       * @brief Save the order shown (dragged, or sorted by name) with `POST /api/apps/reorder`.
       *
       * @return {Promise<void>} Completion of the request and reload.
       */
      async saveOrder() {
        const order = this.displayedApps.map(({ app }) => app.uuid).filter(Boolean);
        this.actionDisabled = true;
        this.actionError = "";
        try {
          const r = await apiFetch("./api/apps/reorder", {
            method: "POST",
            headers: {
              "Content-Type": "application/json"
            },
            body: JSON.stringify({ order }),
          });
          if (r.status !== 200) {
            this.actionError = this.$t('apps.reorder_failed') + (await this.errorText(r));
          } else {
            this.sortMode = "default";
          }
        } finally {
          this.actionDisabled = false;
          await this.loadApps();
        }
      },
      addDetached() {
        this.editForm.detached.push("");
      },
      showCoverFinder() {
        // Reset search state
        this.coverCandidates = [];
        this.coverSearchQuery = "";

        this.showStacked(this.$refs.coverFinderModal);

        // Perform initial search with app name
        this.performCoverSearch();
      },
      performCoverSearch() {
        this.coverSearching = true;
        this.coverCandidates = [];

        // Use search query if provided, otherwise fall back to app name
        const searchTerm = this.coverSearchQuery.trim() || this.editForm["name"].toString();

        searchCovers(searchTerm)
          .then(list => this.coverCandidates = list)
          .finally(() => this.coverSearching = false);
      },
      useCover(cover) {
        this.coverFinderBusy = true;
        apiFetch("./api/covers/upload", {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            key: cover.key,
            url: cover.saveUrl,
          })
        }).then(r => {
          if (!r.ok) throw new Error("Failed to download covers");
          return r.json();
        }).then(body => {
          this.editForm["image-path"] = body.path;
          // Close the modal
          const modalEl = this.$refs.coverFinderModal;
          if (modalEl) {
            const modal = Modal.getInstance(modalEl);
            if (modal) {
              modal.hide();
            }
          }
        })
          .finally(() => this.coverFinderBusy = false);
      },
      browseFor(type, titleKey, startPath, callback, acceptedExtensions = null) {
        this.fileBrowserType = type;
        this.fileBrowserTitle = this.$t(titleKey);
        this.fileBrowserCallback = callback;
        this.fileBrowserAcceptedExtensions = acceptedExtensions;
        this.fileBrowserSelectedPath = startPath || '';
        this.fileBrowserTypedPath = startPath || '';
        this.fileBrowserError = '';
        this.fileBrowserNavigate(startPath || '');
        this.showStacked(this.$refs.fileBrowserModal);
      },
      fileBrowserClose() {
        const modal = Modal.getInstance(this.$refs.fileBrowserModal);
        if (modal) modal.hide();
      },
      fileBrowserConfirm() {
        const path = this.fileBrowserSelectedPath || this.fileBrowserTypedPath;
        if (path) {
          if (this.fileBrowserAcceptedExtensions && this.fileBrowserType !== 'directory') {
            const lowerPath = path.toLowerCase();
            const isValid = this.fileBrowserAcceptedExtensions.some(ext => lowerPath.endsWith(ext.toLowerCase()));
            if (!isValid) {
              this.fileBrowserError = this.$t('file_browser.error_invalid_extension', { extensions: this.fileBrowserAcceptedExtensions.join(', ') });
              return;
            }
          }
          if (this.fileBrowserCallback) {
            this.fileBrowserCallback(path);
            this.fileBrowserCallback = null;
          }
          this.fileBrowserClose();
        }
      },
      /**
       * @brief Load a directory listing and display any browsing failure.
       *
       * @param {string} path Directory path, or an empty string for the default location.
       * @return {Promise<void>} Completion of the directory request and loading-state update.
       */
      async fileBrowserNavigate(path) {
        this.fileBrowserLoading = true;
        this.fileBrowserError = '';
        const params = new URLSearchParams({ type: this.fileBrowserType });
        if (path) params.set('path', path);
        try {
          const response = await fetch(`./api/browse?${params.toString()}`);
          const data = await response.json();
          if (!response.ok) {
            throw new Error(data.error || 'Browse failed');
          }
          this.fileBrowserCurrentPath = data.path ?? '';
          this.fileBrowserParentPath = data.parent ?? '';
          this.fileBrowserEntries = data.entries ?? [];
          this.fileBrowserTypedPath = data.path ?? '';
          this.fileBrowserSelectedPath = this.fileBrowserType === 'directory' ? (data.path ?? '') : '';
        } catch (err) {
          this.fileBrowserError = err.message;
        } finally {
          this.fileBrowserLoading = false;
        }
      },
      fileBrowserNavigateUp() {
        this.fileBrowserNavigate(this.fileBrowserParentPath);
      },
      fileBrowserSelectEntry(entry) {
        if (entry.type === 'directory') {
          this.fileBrowserNavigate(entry.path);
        } else {
          this.fileBrowserSelectedPath = entry.path;
          this.fileBrowserTypedPath = entry.path;
        }
      },
      fileBrowserActivateEntry(entry) {
        if (entry.type === 'directory') {
          this.fileBrowserNavigate(entry.path);
        } else {
          this.fileBrowserSelectedPath = entry.path;
          this.fileBrowserTypedPath = entry.path;
          this.fileBrowserConfirm();
        }
      },
      fileBrowserOnTypedInput() {
        this.fileBrowserSelectedPath = this.fileBrowserTypedPath;
      },
      browseCmd(type, index, field) {
        const current = this.editForm[`${type}-cmd`][index][field] || '';
        this.browseFor('executable', 'file_browser.select_executable', current, (path) => {
          this.editForm[`${type}-cmd`][index][field] = path;
        });
      },
      browsePrep(index, field) {
        this.browseCmd('prep', index, field);
      },
      browseDetached(index) {
        const current = this.editForm.detached[index] || '';
        this.browseFor('executable', 'file_browser.select_executable', current, (path) => {
          this.editForm.detached[index] = path;
        });
      },
      /**
       * @brief Save the edited application. Applications are identified by UUID; one saved before UUIDs existed
       * is identified by its position, and the server gives it a UUID.
       *
       * @return {Promise<void>|undefined} Completion of the request, or nothing when validation fails.
       */
      save() {
        this.editFormError = "";
        if (typeof this.editForm.name === "string") {
          this.editForm.name = this.editForm.name.trim();
        }
        if (this.editForm["exit-timeout"] !== undefined) {
          const timeout = Number.parseInt(this.editForm["exit-timeout"], 10);
          this.editForm["exit-timeout"] = Number.isNaN(timeout) ? 5 : timeout;
        }
        if (this.editForm["scale-factor"] !== undefined) {
          this.editForm["scale-factor"] = Number.parseInt(this.editForm["scale-factor"], 10) || 100;
        }
        const currentImagePath = this.editForm["image-path"] == null ? "" : String(this.editForm["image-path"]).trim();
        this.editForm["image-path"] = currentImagePath.replaceAll('"', '');

        const imagePath = this.editForm["image-path"];
        if (!imagePath) {
          delete this.editForm["image-path"];
        } else if (!imagePath.toLowerCase().endsWith('.png')) {
          this.editFormError = this.$t('file_browser.error_invalid_extension', { extensions: '.png' });
          const modalBody = this.$refs.editModal.querySelector('.modal-body');
          if (modalBody) modalBody.scrollTop = 0;
          return;
        }

        const body = { ...this.editForm };
        if (body.uuid) {
          delete body.index;
        } else {
          delete body.uuid;
        }

        return apiFetch("./api/apps", {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify(body),
        }).then(async (r) => {
          if (r.status === 200) {
            this.closeEditModal();
            await this.loadApps();
          } else {
            this.editFormError = this.$t('apps.save_failed') + (await this.errorText(r));
          }
        });
      },
      handleImageError(event) {
        // Hide the broken image and show placeholder instead
        event.target.style.display = 'none';
        const placeholder = event.target.nextElementSibling;
        if (placeholder && placeholder.classList.contains('app-poster-placeholder')) {
          placeholder.style.display = 'flex';
        }
      },
      resetSearchQuery() {
        this.searchQuery = "";
      },
      toggleSort() {
        // Sorting goes default -> ascending -> descending -> default
        if (this.sortMode === "default")
          this.sortMode = "asc";
        else if (this.sortMode === "asc")
          this.sortMode = "desc";
        else
          this.sortMode = "default";
      },
      openEditModal() {
        // Use $nextTick because if the edit version of the modal is created too early, it breaks...
        this.$nextTick(() => {
          Modal.getOrCreateInstance(this.$refs.editModal).show();
        });
      },
      closeEditModal() {
        const modal = Modal.getInstance(this.$refs.editModal);
        if (modal) modal.hide();
      },
      // We need to update the z-index since bootstrap gives all modals the same z-index
      // Fixes the stacking issue of modals (cover finder / file browser)
      showStacked(modalEl) {
        modalEl.addEventListener('show.bs.modal', () => {
          const openCount = document.querySelectorAll('.modal.show').length;
          const z = 1055 + (openCount + 1) * 20;
          modalEl.style.zIndex = z;

          requestAnimationFrame(() => {
            const backdrops = document.querySelectorAll('.modal-backdrop');
            const backdrop = backdrops[backdrops.length - 1];
            if (backdrop) backdrop.style.zIndex = z - 10;
          });
        }, { once: true });
        Modal.getOrCreateInstance(modalEl).show();
      },
    },
  }
</script>

<style scoped>
.app-dragover .app-card {
  outline: 2px dashed var(--bs-primary);
  outline-offset: 2px;
}

.app-drag-handle {
  cursor: grab;
}
</style>
