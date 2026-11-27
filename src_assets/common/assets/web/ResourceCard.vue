<template>
    <div class="card">
        <div class="card-body">
            <h2>{{ $t('resource_card.resources') }}</h2>
            <p>{{ $t('resource_card.resources_desc') }}</p>
            <div class="d-flex flex-wrap gap-2 mt-4">
                <a class="btn btn-info" :href="links.documentation" target="_blank" rel="noopener noreferrer">
                  <BookOpen :size="18" class="icon"></BookOpen>
                  {{ $t('resource_card.documentation') }}
                </a>
                <a class="btn btn-secondary" :href="links.repository" target="_blank" rel="noopener noreferrer">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  {{ $t('resource_card.github_repository') }}
                </a>
                <a class="btn btn-secondary" :href="links.wiki" target="_blank" rel="noopener noreferrer">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  {{ $t('resource_card.github_wiki') }}
                </a>
                <a class="btn btn-secondary" :href="links.stutteringClinic" target="_blank" rel="noopener noreferrer">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  {{ $t('resource_card.github_stuttering_clinic') }}
                </a>
            </div>
        </div>
    </div>
    <!-- Legal -->
    <div class="card mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.legal') }}</h2>
            <p>{{ $t('resource_card.legal_desc') }}</p>
            <div class="d-flex flex-wrap gap-2 mt-4">
                <a class="btn btn-danger" :href="links.license" target="_blank" rel="noopener noreferrer">
                  <FileText :size="18" class="icon"></FileText>
                  {{ $t('resource_card.license') }}
                </a>
                <a class="btn btn-danger" :href="links.notice" target="_blank" rel="noopener noreferrer">
                  <AlertCircle :size="18" class="icon"></AlertCircle>
                  {{ $t('resource_card.third_party_notice') }}
                </a>
            </div>
        </div>
    </div>
</template>

<script>
import {
  AlertCircle,
  BookOpen,
  FileText,
} from '@lucide/vue'
import SimpleIcon from './SimpleIcon.vue'
import { APOLLO_WIKI_URL, FORK_URL, apolloWikiPage, forkFile } from './links'

export default {
  props: {
    // Kept for callers that pass it; the fork documents one version only.
    installedVersionNotStable: {
      type: Boolean,
      default: false
    }
  },
  components: {
    SimpleIcon,
    AlertCircle,
    BookOpen,
    FileText,
  },
  data() {
    return {
      links: {
        documentation: `${FORK_URL}#readme`,
        repository: FORK_URL,
        wiki: APOLLO_WIKI_URL,
        stutteringClinic: apolloWikiPage('Stuttering-Clinic'),
        license: forkFile('LICENSE'),
        notice: forkFile('NOTICE'),
      },
    }
  },
}
</script>
