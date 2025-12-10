#include <dobby.h>
#include <unistd.h>
#include "sensor_hook.h"
#include "logging.h"
#include "elf_util.h"
#include "dobby_hook.h"
#include <fstream>
#include <string>
#include <chrono>
#include <sstream>
#include <cmath>

#define LIBSF_PATH "/system/lib64/libsensorservice.so"

extern bool enableSensorHook;

// Standard Android sensors_event_t layout
typedef struct {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        union {
            float           data[16];
            uint64_t        step_counter;
        };
        union {
            float           x;
            float           y;
            float           z;
        } acceleration;
        union {
            float           x;
            float           y;
            float           z;
        } magnetic;
        union {
            float           x;
            float           y;
            float           z;
        } orientation;
        union {
            float           x;
            float           y;
            float           z;
        } gyro;
    };
    uint32_t flags;
    uint32_t reserved1[3];
} sensors_event_t;

// _ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventm
OriginalSensorEventQueueWriteType OriginalSensorEventQueueWrite = nullptr;

OriginalConvertToSensorEventType OriginalConvertToSensorEvent = nullptr;

// Global state for simulation
static bool gEnable = false;
static double gSpeed = 0.0;
static double gBearing = 0.0;
static uint64_t gVirtualSteps = 0;
static uint64_t gStartTimestamp = 0;
static uint64_t gLastConfigUpdateTime = 0;

static uint64_t getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

// Simple config reader
void updateConfig() {
    uint64_t now = getCurrentTimeMs();
    if (now - gLastConfigUpdateTime < 1000) { // Throttle: 1s
        return;
    }
    gLastConfigUpdateTime = now;

    std::ifstream file("/data/local/tmp/portal_config.json");
    if (!file.is_open()) {
        static bool loggedError = false;
        if (!loggedError) {
             LOGE("Native Hook: Failed to open config file /data/local/tmp/portal_config.json");
             loggedError = true;
        }
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto parseBool = [&](const std::string& key) -> bool {
        size_t pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return false;
        size_t colon = content.find_first_of(":", pos);
        if (colon == std::string::npos) return false;
        size_t valueStart = content.find_first_not_of(" \t\n\r", colon + 1);
        if (valueStart == std::string::npos) return false;
        if (content.substr(valueStart, 4) == "true") return true;
        return false;
    };

    auto parseDouble = [&](const std::string& key) -> double {
        size_t pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0.0;
        size_t colon = content.find_first_of(":", pos);
        if (colon == std::string::npos) return 0.0;
        size_t valueStart = content.find_first_not_of(" \t\n\r", colon + 1);
        if (valueStart == std::string::npos) return 0.0;
        size_t valueEnd = content.find_first_of(",}", valueStart);
        if (valueEnd == std::string::npos) valueEnd = content.length();
        std::string val = content.substr(valueStart, valueEnd - valueStart);
        try {
            return std::stod(val);
        } catch (...) {
            return 0.0;
        }
    };

    bool newEnable = parseBool("enable");
    if (newEnable != gEnable) {
        LOGD("Native Hook: State changed to %d", newEnable);
    }
    gEnable = newEnable;
    gSpeed = parseDouble("speed");
    gBearing = parseDouble("bearing");
}

void updateSensorConfig(bool enable, double speed, double bearing) {
    if (gEnable != enable) {
        LOGD("Native Hook: JNI Update State to %d", enable);
    }
    gEnable = enable;
    gSpeed = speed;
    gBearing = bearing;
}


int64_t SensorEventQueueWrite(void *tube, void *events, int64_t numEvents) {
    if (enableSensorHook && events != nullptr) {
        updateConfig();
        
        if (gEnable) {
            sensors_event_t* sensorEvents = (sensors_event_t*)events;
            for (int i = 0; i < numEvents; i++) {
                sensors_event_t& event = sensorEvents[i];
                
                // Debug log for first event of batch to confirm hook is active
                if (i == 0) {
                   LOGD("Native Hook: Processing batch of %ld events. First Type: %d", numEvents, event.type);
                }

                if (event.type == 1) { // Accelerometer
                     if (gSpeed > 0.1) {
                         double freq = gSpeed * 1.4;
                         double t = event.timestamp / 1000000000.0;
                         double phase = t * freq * 2 * M_PI;
                         double jitter = ((event.timestamp / 1000000) % 97) / 100.0;
                         
                         double baseWave = 2.0 * sin(phase) + 0.5 * sin(2 * phase + 0.5);
                         double noise = 0.2 * jitter;
                         
                         event.acceleration.x = (float)(sin(phase * 0.5) * 0.5 + noise);
                         event.acceleration.y = (float)(cos(phase * 0.5) * 0.3);
                         event.acceleration.z = (float)(9.8 + baseWave + noise);
                     }
                }
                else if (event.type == 2) { // Magnetic Field
                     if (gEnable) {
                         double bearingRad = gBearing * M_PI / 180.0;
                         double magStrength = 40.0;
                         double jitter = ((event.timestamp / 1000000) % 97) / 1000.0;
                         double sway = 0.0;
                         if (gSpeed > 0.1) {
                             double freq = gSpeed * 1.4;
                             double t = event.timestamp / 1000000000.0;
                             sway = sin(t * freq * 0.5) * 0.05;
                         }
                         double localBearing = -bearingRad + sway + jitter;
                         
                         event.magnetic.x = (float)(magStrength * sin(localBearing));
                         event.magnetic.y = (float)(magStrength * cos(localBearing));
                         event.magnetic.z = (float)(-30.0 + jitter);
                     }
                }
                else if (event.type == 4) { // Gyroscope
                     if (gSpeed > 0.1) {
                         double freq = gSpeed * 1.4;
                         double omega = freq * 0.5 * 2 * M_PI;
                         double t = event.timestamp / 1000000000.0;
                         double amplitude = 0.05;
                         double yawRate = amplitude * omega * cos(omega * t);
                         double jitter = ((event.timestamp / 1000000) % 53) / 1000.0;
                         
                         event.gyro.x = (float)(jitter); 
                         event.gyro.y = (float)(jitter);
                         event.gyro.z = (float)(yawRate + jitter);
                     }
                }
                else if (event.type == 19) { // Step Counter
                     // Accumulate steps based on time delta to handle variable speed smoothly
                     if (gStartTimestamp == 0) gStartTimestamp = event.timestamp;
                     
                     static double gStepAccumulator = 0.0;
                     static uint64_t gLastStepEventTime = 0;
                     
                     if (gLastStepEventTime == 0) gLastStepEventTime = event.timestamp;
                     
                     double dt = (event.timestamp - gLastStepEventTime) / 1000000000.0; // ns to s
                     if (dt > 0 && gSpeed > 0.1) {
                         // Frequency ~ speed * 1.4 (heuristic)
                         double currentFreq = gSpeed * 1.4;
                         gStepAccumulator += dt * currentFreq;
                     }
                     
                     gVirtualSteps = (uint64_t)gStepAccumulator;
                     gLastStepEventTime = event.timestamp;

                     event.step_counter = gVirtualSteps;
                } 
                else if (event.type == 18) { // Step Detector
                     if (gSpeed > 0.5) event.data[0] = 1.0f;
                }
            }
        }
    }
    return OriginalSensorEventQueueWrite(tube, events, numEvents);
}

void ConvertToSensorEvent(void *src, void *dst) {
    OriginalConvertToSensorEvent(src, dst);
}

void doSensorHook() {
    LOGD("Native Hook: doSensorHook() called");
    SandHook::ElfImg sensorService(LIBSF_PATH);
    if (!sensorService.isValid()) {
        LOGE("failed to load libsensorservice");
        return;
    }
    auto sensorWrite = sensorService.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventm");
    if (sensorWrite == nullptr) {
        sensorWrite = sensorService.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventj");
    }
    if (sensorWrite != nullptr) {
        LOGD("Dobby SensorEventQueue::write found at %p", sensorWrite);
        OriginalSensorEventQueueWrite = (OriginalSensorEventQueueWriteType)InlineHook(sensorWrite, (void *)SensorEventQueueWrite);
        if (OriginalSensorEventQueueWrite != nullptr) {
            LOGD("Native Hook: Successfully hooked SensorEventQueue::write, original at %p", OriginalSensorEventQueueWrite);
        } else {
            LOGE("Native Hook: InlineHook returned null for SensorEventQueue::write");
        }
    } else {
        LOGE("Failed to find SensorEventQueue::write");
    }
}
