#define DEMO_SUBNET_PING        0
#define DEMO_CAM_STREAM         1

#if DEMO_SUBNET_PING == 1

#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_wifi.h"

#include "app_wifi.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/mem.h"
#include "lwip/icmp.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define PING_DATA_SIZE 32

#define PING_ID        0xAFAF

#define inet_addr_from_ipaddr(target_inaddr, source_ipaddr) ((target_inaddr)->s_addr = source_ipaddr->u_addr.ip4.addr)

#define inet_addr_to_ipaddr(target_ipaddr, source_inaddr) ((target_ipaddr)->u_addr.ip4.addr = (source_inaddr)->s_addr)

static u16_t ping_seq_num;
static u32_t ping_time;

static void ping_prepare_echo( struct icmp_echo_hdr *iecho, u16_t len) {
    size_t i;
    size_t data_len = len - sizeof(struct icmp_echo_hdr);

    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0;
    iecho->id     = PING_ID;
    iecho->seqno  = htons(++ping_seq_num);

    /* fill the additional data buffer with some data */
    for(i = 0; i < data_len; i++) {
        ((char*)iecho)[sizeof(struct icmp_echo_hdr) + i] = (char)i;
    }

    iecho->chksum = inet_chksum(iecho, len);
}

/* Ping using the socket ip */
static err_t ping_send(int s, ip_addr_t *addr) {
  int err;
  struct icmp_echo_hdr *iecho;
  struct sockaddr_in to;
  size_t ping_size = sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE;

  iecho = (struct icmp_echo_hdr *)mem_malloc((mem_size_t)ping_size);
  if (!iecho) {
      ESP_LOGI("PING", "Mem err");
    return ERR_MEM;
  }

  ping_prepare_echo(iecho, (u16_t)ping_size);

  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  inet_addr_from_ipaddr(&to.sin_addr, addr);

  err = lwip_sendto(s, iecho, ping_size, 0, (struct sockaddr*)&to, sizeof(to));

  mem_free(iecho);

  return (err ? ERR_OK : ERR_VAL);
}

static int ping_recv(int s) {
    char buf[64];
    int fromlen, len;
    struct sockaddr_in from;
    struct ip_hdr *iphdr;
    struct icmp_echo_hdr *iecho;

    while((len = lwip_recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&from, (socklen_t*)&fromlen)) > 0) {
        if (len >= (int)(sizeof(struct ip_hdr)+sizeof(struct icmp_echo_hdr))) {
            ip_addr_t fromaddr;
            inet_addr_to_ipaddr(&fromaddr, &from.sin_addr);
            //ESP_LOGI("PING", "ping: recv %d ms\n", (sys_now() - ping_time));

            iphdr = (struct ip_hdr *)buf;
            iecho = (struct icmp_echo_hdr *)(buf + (IPH_HL(iphdr) * 4));
            if ((iecho->id == PING_ID) && (iecho->seqno == htons(ping_seq_num))) {
                //ESP_LOGI("PING", "ping: ok");
                return (sys_now() - ping_time);
            } 
            else {
                //ESP_LOGI("PING", "ping: drop");
            }
        }
    }

    if (len == 0) {
        //ESP_LOGI("PING", "ping: recv - %d ms - timeout", (sys_now()-ping_time));
        return -1;
    }

    return -1;
}

typedef struct {
    uint8_t thread_index;
    uint8_t thread_done;
    ip_addr_t adr;
    uint8_t adr_start;
    uint8_t adr_len;
    int8_t *result_buf;
    int timeout_ms;
} task_parameters_t;

static void ping_subtask(void *param) {
    task_parameters_t *prm = (task_parameters_t *)param;

    //ESP_LOGI("PING", "%d: Timeout: %d", prm->thread_index, prm->timeout_ms);
    //ESP_LOGI("PING", "%d: Adr: %s", prm->thread_index, ipaddr_ntoa(&prm->adr));
    //ESP_LOGI("PING", "%d: Start: %d", prm->thread_index, prm->adr_start);
    //ESP_LOGI("PING", "%d: Length: %d", prm->thread_index, prm->adr_len);

    // create socket
    int s = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (s < 0) {
        ESP_LOGI("PING", "Sock err");
        return;
    }
    //
    else {
        // set receive timeout
        struct timeval timeout;
        timeout.tv_sec = prm->timeout_ms / 1000;
        timeout.tv_usec = (prm->timeout_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(struct timeval));

        // loop over addresses
        for (uint16_t i = prm->adr_start; i < prm->adr_start + prm->adr_len; i++) {
            // sanity check
            if (i > 254)
                break;
            
            // set next index
            uint32_t mask = prm->adr.u_addr.ip4.addr;
            mask &= 0x00ffffff;
            mask |= i << 24;
            prm->adr.u_addr.ip4.addr = mask;

            // send ping is ok?
            if (ping_send(s, &prm->adr) == ERR_OK) {
                //ESP_LOGI("PING", "ping send ok");
                ping_time = sys_now();
                int ping_rsp = ping_recv(s);

                // ping receive is ok?
                if (ping_rsp > -1) {
                    ESP_LOGI("PING", "%d: Adr %d, %d ms", prm->thread_index, i, ping_rsp);

                    // all is good, save the ping time
                    prm->result_buf[i - prm->adr_start] = (ping_rsp > 100) ? 100 : ping_rsp;
                }
            }
        }
    }

    // we are done
    prm->thread_done = 1;

    // delete the thread
    vTaskDelete(NULL);
}

static int8_t ping_result[255];

int8_t *ping_subnet(ip_addr_t *adr, int timeout_ms, uint8_t threads) {
    // clear ping result buffer
    memset(ping_result, -1, 255);

    // get some memory
    task_parameters_t *params = malloc(sizeof(task_parameters_t) * threads);

    // calculate pings per thread
    uint8_t ping_per_thread = (254 / threads) + 1;

    // loop over each thread
    for (uint8_t i = 0; i < threads; i++) {
        // pil parameters
        params[i].thread_index = i;
        params[i].thread_done = 0;
        memcpy(&params[i].adr, adr, sizeof(ip_addr_t));
        params[i].adr_start = 1 + (ping_per_thread * i);
        params[i].adr_len = ping_per_thread;
        params[i].result_buf = &ping_result[params[i].adr_start];
        params[i].timeout_ms = timeout_ms;

        // start task
        xTaskCreate(ping_subtask, "ping", 2048, &params[i], 2, NULL);
    }

    // wait for all threads to finish
    while (1) {
        vTaskDelay(timeout_ms / 10);
        uint8_t thread_running = 0;
        for (uint8_t i = 0; i < threads; i++) {
            if (!params[i].thread_done)
                thread_running++;
        }
        if (thread_running == 0)
            break;
    }

    // free the memory
    free(params);

    // return the result array
    return ping_result;
}

void app_main(void) {
    // initialize flash
    app_flash_init();

    // initialize netif and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_sta("network", "");

    while (!wifi_sta_is_connected())
        vTaskDelay(100);

    // build address from string
    ip_addr_t adr;
    ipaddr_aton("192.168.178.1", &adr);

    // ping subnet
    int8_t *res = ping_subnet(&adr, 100, 1);

    // print result
    for (int i = 0; i < 255; i++) {
        if (res[i] != -1)
            ESP_LOGI("PING", "%d: %d", i, res[i]);
    }

    // done
    ESP_LOGI("PING", "scan done");
}
#endif

#if DEMO_CAM_STREAM == 1

#include "app_wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sys/param.h"
#include <string.h>
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_camera.h"

#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 32
#define SIOD_GPIO_NUM 13
#define SIOC_GPIO_NUM 12
#define Y9_GPIO_NUM 39
#define Y8_GPIO_NUM 36
#define Y7_GPIO_NUM 23
#define Y6_GPIO_NUM 18
#define Y5_GPIO_NUM 15
#define Y4_GPIO_NUM 4
#define Y3_GPIO_NUM 14
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 27
#define HREF_GPIO_NUM 25
#define PCLK_GPIO_NUM 19

static const char *TAG = "example:take_picture";

static camera_config_t camera_config = {
    .pin_d0 = Y2_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 10000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_RGB565, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_HD,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 12, //0-63 lower number means higher quality
    .fb_count = 2,       //if more than one, i2s runs in continuous mode. Use only with JPEG
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static esp_err_t init_camera()
{
    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",
            (uint32_t)(_jpg_buf_len/1024),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time);
    }

    last_frame = 0;
    return res;
}

void app_main()
{
    // initialize flash
    app_flash_init();

    // initialize netif and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_sta("network", "");

    while (!wifi_sta_is_connected())
        vTaskDelay(100);

    if (ESP_OK != init_camera()) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t stream_httpd = NULL;
    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = jpg_stream_httpd_handler,
        .user_ctx  = NULL
    };
    
    //Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &index_uri);
    }
}
#endif