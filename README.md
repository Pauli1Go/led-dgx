# LED DGX

ESP32 firmware for WS2815 LED strips that visualizes GPU utilization on NVIDIA DGX systems. The firmware retrieves Prometheus metrics from DCGM Exporter and assigns one LED-strip zone to each DGX endpoint.

## Data source

The NVIDIA DCGM Exporter is used as the data source:

```text
nvcr.io/nvidia/k8s/dcgm-exporter:4.6.0-4.8.3-distroless
```

Start the exporter on every DGX system to be monitored:

```bash
docker run -d \
  --name dcgm-exporter \
  --restart unless-stopped \
  --gpus all \
  --cap-add SYS_ADMIN \
  -p 9400:9400 \
  nvcr.io/nvidia/k8s/dcgm-exporter:4.6.0-4.8.3-distroless \
  --collect-interval 5000
```

The exporter exposes its metrics at:

```text
http://<DGX-IP-ADDRESS>:9400/metrics
```

Replace `<DGX-IP-ADDRESS>` with the IP address of the DGX system. The ESP32 evaluates `DCGM_FI_DEV_GPU_UTIL` and calculates the average across every GPU reported by an endpoint.

## LED display

Each LED strip has two zones with 13 LEDs each. Each zone is assigned to one DGX endpoint.

| State | Display |
| --- | --- |
| Endpoint unreachable or not configured | Off |
| GPU utilization 0-5% | Pulsing blue |
| GPU utilization 6-100% | Moving animation from green (low) to red (high) |

Up to eight LED strips are supported.

## Requirements

- ESP32 Lolin D32
- PlatformIO
- WS2815 LED strips
- Wi-Fi access from the ESP32 to the DCGM Exporter endpoints
- Docker with NVIDIA Container Toolkit on every DGX system

## Set up the firmware

1. Create a local `.env.local` file in the project root:

   ```dotenv
   WIFI_SSID="My-Wi-Fi"
   WIFI_PASSWORD="My-Wi-Fi-Password"
   WEBUI_USERNAME="admin"
   WEBUI_PASSWORD="Secure-Password"
   ```

2. Build and upload the firmware to the ESP32:

   ```bash
   platformio run --target upload
   ```

3. Upload the web interface from `data/` to LittleFS:

   ```bash
   platformio run --target uploadfs
   ```

4. Open the serial monitor and browse to the printed IP address:

   ```bash
   platformio device monitor
   ```

## Configuration

### Web interface

Open the ESP32 IP address in a browser and sign in with `WEBUI_USERNAME` and `WEBUI_PASSWORD` from `.env.local`. Configure the LED-strip count, GPIO pins, and two DGX URLs for each strip. Enter the complete metrics endpoint as the URL, for example:

```text
http://192.168.1.20:9400/metrics
```

The configuration is stored persistently on the ESP32. The device restarts after saving.

### Serial interface

Connect to the ESP32 at 115200 baud, for example with PlatformIO:

```bash
platformio device monitor
```

Sign in with the same credentials as the web interface, then add an LED strip. The two DGX URLs map to the two LED zones of the strip:

```text
login admin Secure-Password
add DGX-Rack-1|18|http://192.168.1.20:9400/metrics|http://192.168.1.21:9400/metrics
```

Useful serial commands:

```text
help
status
interval 5
set 0 DGX-Rack-1|18|http://192.168.1.20:9400/metrics|http://192.168.1.21:9400/metrics
remove 0
refresh
factory-reset
reboot
logout
```

`add`, `set`, `remove`, and `factory-reset` save the configuration. Changes to LED strips restart the ESP32 automatically.

## Development

```bash
platformio run
```

The PlatformIO environment is named `lolin_d32`. Serial communication uses 115200 baud.
