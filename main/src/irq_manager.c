#include "irq_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bq25628.h"
#include "TCA8418.h"

static const char *TAG = "IRQ_MGR";
static TaskHandle_t s_irq_task_handle = NULL;

/**
 * @brief Rutina de interrupción genérica en IRAM para todos los GPIOs registrados.
 */
static void IRAM_ATTR gpio_generic_isr_handler(void *arg){
    uint32_t source_bit = (uint32_t)(uintptr_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (s_irq_task_handle != NULL) {
        xTaskNotifyFromISR(s_irq_task_handle, source_bit, eSetBits, &xHigherPriorityTaskWoken);
    }

    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Manejador de interrupcion del cargador BQ25628
 */
static void handle_bq25628_interrupt(void){
    uint8_t chg_flag_0 = 0;

    // Verificar si la interrupción fue por fin de conversión ADC
    if (bq25628_check_adc_done_flag(&chg_flag_0) == ESP_OK && chg_flag_0) bq25628_notify_adc_done();

    // Leer fallas y estado operativo
    uint8_t faults = BQ25628_FAULT_NONE;
    if (bq25628_read_faults(&faults) == ESP_OK && faults != BQ25628_FAULT_NONE) {
        if (faults & BQ25628_FAULT_SYS) {
            ESP_LOGE(TAG, "¡FALLA CRÍTICA BQ25628: Sobretensión o corto en VSYS!");
        }
        if (faults & BQ25628_FAULT_TSHUT) {
            ESP_LOGE(TAG, "¡FALLA CRÍTICA BQ25628: Apagado térmico (TSHUT)!");
        }
    }

    bq25628_status_t status;
    if (bq25628_read_status(&status) == ESP_OK) {
        ESP_LOGI(TAG, "Estado BQ25628 -> VBUS: %s | CHG_STAT: %d", status.vbus_present ? "PRESENTE" : "AUSENTE", status.chg_stat);
    }
}

/**
 * @brief Manejador de interrupcion del Fuel Gauge BQ27441
 */
static void handle_bq27441_interrupt(void){
	if(BQ27441_fcFlag()) battery_info.fully_charged = true;
	else battery_info.fully_charged = false;

	if(BQ27441_socFlag()) battery_info.low_battery = true;
	else battery_info.low_battery = false;

	if(BQ27441_socfFlag()) battery_info.crit_low_battery = true;
	else battery_info.crit_low_battery = false;

	battery_info.StateOfCharge = BQ27441_soc(FILTERED); // Actualizar porcentaje de bateria
}


/**
 * @brief Manejador de interrupción del controlador de teclado TCA8418.
 *
 * Extrae todos los eventos pendientes en la FIFO interna (presión y liberación
 * de teclas) y limpia las banderas del registro INT_STAT (0x02).
 */
static void handle_tca8418_interrupt(void)
{
    tca8418_key_event_t event;
    bool has_event = false;

    /* 1. Drenar todos los eventos acumulados en la FIFO (máximo 10) */
    while (tca8418_read_event(&event, &has_event) == ESP_OK && has_event) {
        ESP_LOGI(TAG, "Tecla %u (Fila %u, Col %u) -> %s",
                 event.key_code,
                 event.row,
                 event.col,
                 event.is_pressed ? "PRESIONADA" : "LIBERADA");

        /* Aquí se enviará el evento a la cola de interfaz de usuario / menú LCD */
    }

    /* 2. Leer y limpiar las banderas de interrupción en INT_STAT (W1C) */
    tca8418_int_status_t int_stat;
    if (tca8418_get_and_clear_int_status(&int_stat, true) == ESP_OK) {
        if (int_stat.overflow) {
            ESP_LOGW(TAG, "Advertencia TCA8418: Desbordamiento de FIFO de teclado");
        }
    }
}


/**
 * @brief Tarea central que atiende todas las interrupciones de periféricos.
 */
static void irq_manager_task(void *pvParameters){
    uint32_t pending_irqs = 0;

    while (1) {
        // Bloquea sin consumir CPU hasta que alguna ISR encienda al menos un bit
        if (xTaskNotifyWait(0x00, UINT32_MAX, &pending_irqs, portMAX_DELAY) == pdTRUE) {

            if (pending_irqs & IRQ_SRC_CHARGER_INT) {
                handle_bq25628_interrupt();
            }

            if (pending_irqs & IRQ_SRC_FUEL_GAUGE_INT) {
                handle_bq27441_interrupt();
            }

            if (pending_irqs & IRQ_SRC_BUTTON_INT) {
                handle_tca8418_interrupt();
            }

        }
    }
}

esp_err_t irq_manager_init(void){
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    BaseType_t task_ok = xTaskCreate(irq_manager_task, "irq_mgr_task", 3072, NULL, configMAX_PRIORITIES - 2, &s_irq_task_handle);
    return (task_ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t irq_manager_register_gpio(gpio_num_t gpio_num, gpio_int_type_t intr_type, gpio_pull_mode_t pull_mode, irq_source_t source_bit){
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (pull_mode == GPIO_PULLUP_ONLY || pull_mode == GPIO_PULLUP_PULLDOWN)
                        ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull_mode == GPIO_PULLDOWN_ONLY || pull_mode == GPIO_PULLUP_PULLDOWN)
                        ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = intr_type,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) return ret;

    return gpio_isr_handler_add(gpio_num, gpio_generic_isr_handler, (void *)(uintptr_t)source_bit);
}
