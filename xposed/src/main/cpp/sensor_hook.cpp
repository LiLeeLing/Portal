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
        LOGE("Native Hook: Failed to open config file /data/local/tmp/portal_config.json");
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // ... (parsing logic same as before) ...
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
    
    // ... (parseDouble logic same as before) ...
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


int64_t SensorEventQueueWrite(void *tube, void *events, int64_t numEvents) {
    if (enableSensorHook && events != nullptr) {
        updateConfig();
        
        if (gEnable) {
            sensors_event_t* sensorEvents = (sensors_event_t*)events;
            for (int i = 0; i < numEvents; i++) {
                sensors_event_t& event = sensorEvents[i];
                // Log only once per second to avoid spam, or specific heavy debug
                // LOGD("Native Hook Processing Type: %d", event.type);
                
                // Example: Walking Logic
                if (event.type == 1) { // Accelerometer
                 if (gSpeed > 0.1) {
                     // 1. Basic Rhythm: Freq ~ Speed * 1.4 Hz
                     double freq = gSpeed * 1.4;
                     
                     // 2. Anti-ML: Add Randomness & Harmonics
                     // Use timestamp for phase to ensure continuity
                     double t = event.timestamp / 1000000000.0;
                     double phase = t * freq * 2 * M_PI;
                     
                     // Pseudo-random jitter based on time (milliseconds part) prevents perfect periodicity
                     // Modulo 97 is just a prime number to scramble modulation
                     double jitter = ((event.timestamp / 1000000) % 97) / 100.0; // 0.0 to 0.96
                     
                     // Z-axis: Gravity (9.8) + Main Step Wave + 2nd Harmonic (Asymmetry) + Noise
                     // Harmonic: 0.3 * sin(2*phase) makes the 'step' impact sharper than the 'lift'
                     double baseWave = 2.0 * sin(phase) + 0.5 * sin(2 * phase + 0.5);
                     double noise = 0.2 * jitter; // High freq noise
                     
                     event.acceleration.x = (float)(sin(phase * 0.5) * 0.5 + noise); // Lateral Sway
                     event.acceleration.y = (float)(cos(phase * 0.5) * 0.3);         // Forward/Back adjustments
                     event.acceleration.z = (float)(9.8 + baseWave + noise);         // Vertical Impact
                 }
            }
            else if (event.type == 2) { // Magnetic Field
                 if (gEnable) { // Always override if enabled, even if static
                     // 1. Calculate Field Vector from Bearing
                     // North = 0 deg. Vector (0, 1, 0) in simplified Map mapping? 
                     // Usually Mag points North. If User Bearing is 90 (East), Phone is facing East.
                     // So North is relative to Phone: -90 deg.
                     // Mag Vector Local = Rotate(World_North, -Bearing)
                     
                     double bearingRad = gBearing * M_PI / 180.0;
                     // World North is typically Y+ or Z- depending on location. Let's assume ideal Flat Earth Y+.
                     // Local X = sin(-bearing) * MagStrength
                     // Local Y = cos(-bearing) * MagStrength
                     
                     double magStrength = 40.0; // uT
                     double jitter = ((event.timestamp / 1000000) % 97) / 1000.0;
                     
                     // Add subtle oscillation from walking (Body Sway impacts heading slightly)
                     double sway = 0.0;
                     if (gSpeed > 0.1) {
                         double freq = gSpeed * 1.4;
                         double t = event.timestamp / 1000000000.0;
                         sway = sin(t * freq * 0.5) * 0.05; // +/- 0.05 rad sway
                     }
                     
                     double localBearing = -bearingRad + sway + jitter;
                     
                     event.magnetic.x = (float)(magStrength * sin(localBearing));
                     event.magnetic.y = (float)(magStrength * cos(localBearing));
                     event.magnetic.z = (float)(-30.0 + jitter); // Vertical component (Dip)
                 }
            }
            else if (event.type == 4) { // Gyroscope
                 if (gSpeed > 0.1) {
                     // Gyro measures d(Angle)/dt.
                     // Our Heading Sway is sin(w*t). Derivative is w*cos(w*t).
                     double freq = gSpeed * 1.4; // rad/s of step cycle? No, Hz.
                     double omega = freq * 0.5 * 2 * M_PI; // Sway frequency (half step)
                     
                     double t = event.timestamp / 1000000000.0;
                     // Sway = A * sin(omega * t)
                     // Gyro Z (Yaw rate) = A * omega * cos(omega * t)
                     
                     double amplitude = 0.05; // rad
                     double yawRate = amplitude * omega * cos(omega * t);
                     
                     double jitter = ((event.timestamp / 1000000) % 53) / 1000.0;
                     
                     event.gyro.x = (float)(jitter); 
                     event.gyro.y = (float)(jitter);
                     event.gyro.z = (float)(yawRate + jitter);
                 }
            }
            else if (event.type == 19) { // Step Counter
                 if (gStartTimestamp == 0) {
                     gStartTimestamp = getCurrentTimeMs();
                 }
                 double durationSec = (getCurrentTimeMs() - gStartTimestamp) / 1000.0;
                 if (gSpeed > 0.1) {
                    // Simple linear accumulation: 1m/s ~= 1.4 steps/s
                    gVirtualSteps = (uint64_t)(durationSec * gSpeed * 1.4);
                 }
                 event.step_counter = gVirtualSteps;
                 // LOGD("Native Hook: Overwrote Step Counter to %ld", gVirtualSteps);
            } 
            else if (event.type == 18) { // Step Detector
                 if (gSpeed > 0.5) event.data[0] = 1.0f;
            }
            // Add other sensors here if needed, keeping in mind this is GLOBAL for all apps.
        }
    }
    return OriginalSensorEventQueueWrite(tube, events, numEvents);
}

void ConvertToSensorEvent(void *src, void *dst) {
    OriginalConvertToSensorEvent(src, dst);
    // This function converts internal Event format to sensors_event_t. 
    // It's often used inside the service before writing to the queue.
    // Hooking 'write' above is usually sufficient and safer as it handles the final buffer.
    // We keep this hook minimal or remove it to avoid double-processing.
}

void doSensorHook() {
    SandHook::ElfImg sensorService(LIBSF_PATH);

    if (!sensorService.isValid()) {
        LOGE("failed to load libsensorservice");
        return;
    }

    // Hook SensorEventQueue::write
    auto sensorWrite = sensorService.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventm");
    if (sensorWrite == nullptr) {
        sensorWrite = sensorService.getSymbolAddress<void*>("_ZN7android16SensorEventQueue5writeERKNS_2spINS_7BitTubeEEEPK12ASensorEventj");
    }

    if (sensorWrite != nullptr) {
        LOGD("Dobby SensorEventQueue::write found at %p", sensorWrite);
        OriginalSensorEventQueueWrite = (OriginalSensorEventQueueWriteType)InlineHook(sensorWrite, (void *)SensorEventQueueWrite);
    } else {
        LOGE("Failed to find SensorEventQueue::write");
    }
    
    // Optional: Hook convertToSensorEvent if needed, but 'write' is better for modifying outgoing data.
}
