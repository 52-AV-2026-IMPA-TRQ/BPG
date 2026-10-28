<template>
  <v-container class="pa-4 pa-md-6" fluid>
    <v-row class="mb-2" align="center">
      <v-col cols="12" md="8">
        <div class="text-h4 font-weight-bold">
          Banco de Prueba de Generadores
        </div>

        <div class="text-body-1 text-medium-emphasis mt-1">
          Monitor operativo del banco Start Torque
        </div>
      </v-col>

      <v-col class="d-flex justify-md-end" cols="12" md="4">
        <v-chip
          :color="connected ? 'success' : 'error'"
          size="large"
          variant="flat"
        >
          {{ connected ? 'ESP32 conectado' : 'Sin comunicación' }}
        </v-chip>
      </v-col>
    </v-row>

    <v-alert
      v-if="!connected && error"
      class="mb-4"
      type="error"
      variant="tonal"
    >
      No se pudo actualizar el estado del banco. Se conserva la última
      información recibida, pero no debe considerarse actual.
    </v-alert>

    <v-alert
      v-if="status?.simulationEnabled"
      class="mb-4"
      type="info"
      variant="tonal"
    >
      El sistema está trabajando con datos simulados. Los GPIO de potencia
      todavía no están habilitados.
    </v-alert>

    <v-row>
      <v-col cols="12" lg="4">
        <v-card class="h-100" elevation="3">
          <v-card-title class="d-flex align-center justify-space-between">
            Estado general

            <v-chip
              :color="stateColor"
              size="large"
              variant="flat"
            >
              {{ stateLabel }}
            </v-chip>
          </v-card-title>

          <v-divider />

          <v-card-text>
            <div class="text-overline text-medium-emphasis">
              Fallo activo
            </div>

            <div
              class="text-h5 font-weight-bold"
              :class="status?.fault === 'NONE' ? 'text-success' : 'text-error'"
            >
              {{ faultLabel }}
            </div>

            <v-progress-linear
              class="mt-6"
              color="primary"
              height="18"
              :model-value="safeNumber(status?.pwmPercent)"
              rounded
            >
              <strong>{{ formatNumber(status?.pwmPercent, 1) }} % PWM</strong>
            </v-progress-linear>

            <div class="text-body-2 text-medium-emphasis mt-5">
              Última actualización:
              <strong>{{ formattedLastUpdate }}</strong>
            </div>

            <div class="text-body-2 text-medium-emphasis mt-1">
              Secuencia:
              <strong>{{ status?.sequenceNumber ?? '—' }}</strong>
            </div>
          </v-card-text>

          <v-card-actions>
            <v-btn
              color="primary"
              :loading="loading"
              variant="tonal"
              @click="fetchStatus"
            >
              Actualizar ahora
            </v-btn>
          </v-card-actions>
        </v-card>
      </v-col>

      <v-col cols="12" lg="8">
        <v-row>
          <v-col
            v-for="item in digitalIndicators"
            :key="item.title"
            cols="12"
            sm="6"
          >
            <v-card elevation="2">
              <v-card-text class="d-flex align-center justify-space-between">
                <div>
                  <div class="text-subtitle-1 font-weight-bold">
                    {{ item.title }}
                  </div>

                  <div class="text-body-2 text-medium-emphasis">
                    {{ item.description }}
                  </div>
                </div>

                <v-chip
                  :color="item.color"
                  size="large"
                  variant="flat"
                >
                  {{ item.label }}
                </v-chip>
              </v-card-text>
            </v-card>
          </v-col>
        </v-row>
      </v-col>
    </v-row>

    <v-row class="mt-1">
      <v-col
        v-for="measurement in measurements"
        :key="measurement.title"
        cols="12"
        sm="6"
        lg="3"
      >
        <v-card elevation="2">
          <v-card-text>
            <div class="text-overline text-medium-emphasis">
              {{ measurement.title }}
            </div>

            <div class="text-h4 font-weight-bold">
              {{ measurement.value }}
            </div>

            <div class="text-body-2 text-medium-emphasis">
              {{ measurement.description }}
            </div>
          </v-card-text>
        </v-card>
      </v-col>
    </v-row>

    <v-card class="mt-4" elevation="2">
      <v-card-title>Condición de seguridad</v-card-title>

      <v-divider />

      <v-card-text>
        <v-list density="compact">
          <v-list-item>
            <v-list-item-title>
              La interfaz es exclusivamente de monitorización.
            </v-list-item-title>
          </v-list-item>

          <v-list-item>
            <v-list-item-title>
              PWM y contactora permanecen bajo control del firmware.
            </v-list-item-title>
          </v-list-item>

          <v-list-item>
            <v-list-item-title>
              Los controles físicos y las protecciones no dependen del navegador.
            </v-list-item-title>
          </v-list-item>
        </v-list>
      </v-card-text>
    </v-card>
  </v-container>
</template>

<script setup>
  import { computed, onMounted } from 'vue'
  import { useBenchStatus } from '@/composables/useApi'

  const {
    status,
    loading,
    connected,
    error,
    lastUpdatedAt,
    fetchStatus,
    startPolling,
  } = useBenchStatus(500)

  const stateLabels = {
    INITIALIZING: 'INICIALIZANDO',
    READY: 'LISTO',
    TESTING: 'ENSAYO',
    COMPLETED: 'COMPLETADO',
    STOPPED: 'DETENIDO',
    FAULT: 'FALLO',
  }

  const stateColors = {
    INITIALIZING: 'info',
    READY: 'success',
    TESTING: 'warning',
    COMPLETED: 'success',
    STOPPED: 'grey',
    FAULT: 'error',
  }

  const faultLabels = {
    NONE: 'Sin fallos',
    FC1_SECURITY: 'Seguridad FC1',
    TIMEOUT: 'Tiempo máximo excedido',
    OVERCURRENT: 'Sobrecorriente',
    SENSOR: 'Fallo de sensor',
    INTERNAL: 'Fallo interno',
  }

  const stateLabel = computed(() => {
    return stateLabels[status.value?.state] || status.value?.state || 'SIN DATOS'
  })

  const stateColor = computed(() => {
    return stateColors[status.value?.state] || 'grey'
  })

  const faultLabel = computed(() => {
    return faultLabels[status.value?.fault] || status.value?.fault || 'Sin datos'
  })

  const digitalIndicators = computed(() => [
    {
      title: 'FC1 Seguridad',
      description: 'Fin de carrera de protección',
      label: status.value?.fc1Safe ? 'SEGURO' : 'NO SEGURO',
      color: status.value?.fc1Safe ? 'success' : 'error',
    },
    {
      title: 'FC2 Torque',
      description: 'Detección del torque establecido',
      label: status.value?.fc2TorqueReached ? 'ALCANZADO' : 'PENDIENTE',
      color: status.value?.fc2TorqueReached ? 'success' : 'grey',
    },
    {
      title: 'Contactora',
      description: 'Habilitación lógica de potencia',
      label: status.value?.contactorActive ? 'ACTIVA' : 'DESACTIVADA',
      color: status.value?.contactorActive ? 'warning' : 'success',
    },
    {
      title: 'Modo de datos',
      description: 'Origen del estado presentado',
      label: status.value?.simulationEnabled ? 'SIMULACIÓN' : 'HARDWARE',
      color: status.value?.simulationEnabled ? 'info' : 'success',
    },
  ])

  const measurements = computed(() => [
    {
      title: 'PWM',
      value: `${formatNumber(status.value?.pwmPercent, 1)} %`,
      description: 'Ciclo de trabajo actual',
    },
    {
      title: 'Corriente',
      value: `${formatNumber(status.value?.currentA, 2)} A`,
      description: 'Medición instantánea',
    },
    {
      title: 'Corriente máxima',
      value: `${formatNumber(status.value?.maximumCurrentA, 2)} A`,
      description: 'Máximo registrado',
    },
    {
      title: 'Tiempo',
      value: formatElapsed(status.value?.elapsedMs),
      description: 'Duración del ensayo',
    },
  ])

  const formattedLastUpdate = computed(() => {
    if (!lastUpdatedAt.value) {
      return 'Sin datos'
    }

    return lastUpdatedAt.value.toLocaleTimeString('es-AR')
  })

  function safeNumber (value) {
    const number = Number(value)

    if (!Number.isFinite(number)) {
      return 0
    }

    return Math.min(100, Math.max(0, number))
  }

  function formatNumber (value, decimals = 1) {
    const number = Number(value)

    if (!Number.isFinite(number)) {
      return '0'
    }

    return number.toFixed(decimals)
  }

  function formatElapsed (value) {
    const milliseconds = Number(value)

    if (!Number.isFinite(milliseconds) || milliseconds < 0) {
      return '00:00.0'
    }

    const totalSeconds = milliseconds / 1000
    const minutes = Math.floor(totalSeconds / 60)
    const seconds = Math.floor(totalSeconds % 60)
    const tenths = Math.floor((milliseconds % 1000) / 100)

    return `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}.${tenths}`
  }

  onMounted(() => {
    startPolling()
  })
</script>
