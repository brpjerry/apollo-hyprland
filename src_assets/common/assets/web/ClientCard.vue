<template>
  <div class="card">
    <div class="card-body">
      <h2>{{ $t('client_card.clients') }}</h2>
      <p>{{ $t('client_card.clients_desc') }}</p>
      <div class="d-flex flex-wrap gap-2 mt-3">
        <component
          :is="client.link ? 'a' : 'span'"
          v-for="client in clients"
          :key="client.platform"
          class="btn"
          :class="client.link ? 'btn-success' : 'btn-secondary disabled'"
          :href="client.link || undefined"
          :target="client.link ? '_blank' : undefined"
          :rel="client.link ? 'noopener noreferrer' : undefined"
          :aria-disabled="client.link ? undefined : 'true'"
        >
          <component :is="client.icon" :size="18" class="icon"></component>
          <span class="fw-bold">{{ client.platform }}</span>
          [ {{ client.name }} ]
        </component>
      </div>
      <p class="form-text mt-3 mb-0"><i>* {{ $t('client_card.generic_moonlight_clients_desc') }}</i></p>
    </div>
  </div>
</template>

<script setup>
import { Apple, Monitor, Smartphone } from '@lucide/vue'

/** @brief Moonlight clients tuned for Apollo (Artemis). */
const clients = [
  {
    platform: 'Android',
    icon: Smartphone,
    name: 'Artemis',
    link: 'https://github.com/ClassicOldSong/moonlight-android',
  },
  {
    platform: 'iOS',
    icon: Apple,
    name: 'Coming soon...',
    link: '',
  },
  {
    platform: 'Desktop',
    icon: Monitor,
    name: 'Coming soon...',
    link: '',
  },
]
</script>
