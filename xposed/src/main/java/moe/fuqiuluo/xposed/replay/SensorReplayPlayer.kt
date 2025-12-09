package moe.fuqiuluo.xposed.replay

import android.hardware.Sensor
import android.util.Log
import org.json.JSONObject
import java.io.File
import kotlin.math.cos
import kotlin.math.sin

object SensorReplayPlayer {
    private const val TAG = "SensorReplayPlayer"

    // Configuration
    private const val WALKING_FILE_PATH = "/data/local/tmp/motion_0pvsIohTUfTfZ1eBhnodR.json" // Placeholder, will need actual path mapping or file reading strategy
    private const val IDLE_FILE_PATH = "/data/local/tmp/idle.json"
    
    // We might need to read from specific paths if the app can't access arbitrary locations.
    // For now, assuming we can read from where we put them or pass byte arrays.
    // Actually, Xposed modules usually read from their own data dir or we need to pass file descriptors.
    // Ideally, we load this data once on init. 
    // Since we are in the app process (hooked), we need to ensure we can access these files.
    // A safe bet is /data/local/tmp/ for testing, or standardizing on a typically accessible path.
    // Better: Allow initializing with JSON content strings directly to avoid permission issues during this dev phase.

    // State
    private var isInitialized = false
    private val walkingTracks =  mutableMapOf<Int, SensorTrack>()
    private val idleTracks = mutableMapOf<Int, SensorTrack>()

    private var walkingDuration = 0L
    private var idleDuration = 0L

    // Replay State
    private var lastQueryTime = 0L
    private var replayStartTime = 0L
    private var replayOffset = 0L

    // Trajectory Defs
    private const val WALKING_BASE_HEADING = 180.0 // User said they walked South (180)

    data class SensorFrame(
        val timeOffset: Long, // ms from start of track
        val values: FloatArray
    )

    class SensorTrack(val frames: List<SensorFrame>) {
        fun getInterpolatedFrame(offset: Long): FloatArray? {
            // Simple approach: find nearest frame. 
            // For high freq sensors (accel), nearest is usually fine if density is high.
            // If we loop, offset should be modulo totalDuration.
            if (frames.isEmpty()) return null
            
            // Binary search or linear scan? Linear scan with caching index is faster for sequential reads.
            // But 'offset' jumps around. 
            // Let's do a simple binary search for now.
            // Optimization: Assuming uniform distribution is risky.
            
            var low = 0
            var high = frames.size - 1
            
            // Exact bounds check
            if (offset <= frames.first().timeOffset) return frames.first().values
            if (offset >= frames.last().timeOffset) return frames.last().values

            while (low <= high) {
                val mid = (low + high) / 2
                val midVal = frames[mid].timeOffset

                if (midVal < offset) {
                    low = mid + 1
                } else if (midVal > offset) {
                    high = mid - 1
                } else {
                    return frames[mid].values
                }
            }
            // Return closest
            val idx = if (low >= frames.size) high else low
            // Or better: return the one just before to simulates causality? 
            // Nearest neighbor seems best for "playing back".
            return frames[idx].values
        }
    }

    fun init() {
        if (isInitialized) return
        try {
            val walkingFile = File(WALKING_FILE_PATH)
            val idleFile = File(IDLE_FILE_PATH)

            if (walkingFile.exists() && idleFile.exists()) {
                val walkingJson = walkingFile.readText()
                val idleJson = idleFile.readText()

                parseFile(walkingJson, walkingTracks, cropStart = 0L, cropEnd = 60000L).let { walkingDuration = it }
                parseFile(idleJson, idleTracks).let { idleDuration = it }

                replayStartTime = System.currentTimeMillis()
                isInitialized = true
                Log.i(TAG, "Initialized SensorReplay. Walking: ${walkingDuration}ms, Idle: ${idleDuration}ms")
            } else {
                Log.e(TAG, "Sensor files not found at $WALKING_FILE_PATH or $IDLE_FILE_PATH")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to init SensorReplay", e)
        }
    }

    private fun parseFile(jsonStr: String, targetMap: MutableMap<Int, SensorTrack>, cropStart: Long = 0, cropEnd: Long = Long.MAX_VALUE): Long {
        val root = JSONObject(jsonStr)
        val moments = root.getJSONArray("moments")
        
        val tempTracks = mutableMapOf<Int, MutableList<SensorFrame>>()
        var startTime = -1L
        var maxTime = 0L

        for (i in 0 until moments.length()) {
            val moment = moments.getJSONObject(i)
            // elapsed is in seconds (float) based on file view "0.059"
            val elapsedSec = moment.getDouble("elapsed")
            val elapsedMs = (elapsedSec * 1000).toLong()

            if (elapsedMs < cropStart) continue
            if (elapsedMs > cropEnd) break

            if (startTime == -1L) startTime = elapsedMs
            val relativeTime = elapsedMs - startTime
            if (relativeTime > maxTime) maxTime = relativeTime

            val data = moment.optJSONObject("data") ?: continue
            val keys = data.keys()
            while (keys.hasNext()) {
                val sensorTypeStr = keys.next()
                val sensorType = sensorTypeStr.toIntOrNull() ?: continue
                
                val valuesArr = data.getJSONArray(sensorTypeStr)
                val values = FloatArray(valuesArr.length()) { valuesArr.getDouble(it).toFloat() }
                
                tempTracks.getOrPut(sensorType) { mutableListOf() }
                    .add(SensorFrame(relativeTime, values))
            }
        }

        // Convert to immutable tracks
        tempTracks.forEach { (type, list) ->
            targetMap[type] = SensorTrack(list)
        }
        
        return maxTime
    }

    /**
     * Called by hooks.
     * @param sensorType Android Sensor Type (1=Accel, 4=Gyro, etc)
     * @param currentSpeed GPS Speed in m/s. Used to decide Idle vs Walking.
     * @param currentHeading GPS Bearing (0-360). Used to rotate vectors.
     */

    // State Machine for Steps
    private var accumulatedSteps = 0f
    private var lastStateChangeTime = 0L
    private var wasMoving = false

    /**
     * Called by hooks.
     * @param sensorType Android Sensor Type (1=Accel, 4=Gyro, etc)
     * @param currentSpeed GPS Speed in m/s. Used to decide Idle vs Walking.
     * @param currentHeading GPS Bearing (0-360). Used to rotate vectors.
     */
    fun getSensorValues(sensorType: Int, currentSpeed: Double, currentHeading: Double): FloatArray {
        if (!isInitialized) return FloatArray(0)

        // State Machine Update
        if (now - lastQueryTime > 1000) {
            moe.fuqiuluo.xposed.utils.FakeLoc.readConfigFromFile()
            lastQueryTime = now
        }
        
        // Use synchronized FakeLoc state if arguments are default/zero, or always override?
        // SystemSensorManagerHook passes FakeLoc.speed. If FakeLoc is updated, the arg is updated.
        // BUT FakeLoc in this process (App) is updated by the readConfigFromFile above.
        // So the passed `currentSpeed` comes from `FakeLoc` (in SystemSensorManagerHook).
        // Since we update FakeLoc right here, the next call will likely use updated values.
        // However, SystemSensorManagerHook reads FakeLoc.speed BEFORE calling this. 
        // So the update here applies to the NEXT frame. That is fine. 1s latency is acceptable.
        
        // Refetch local state in case it was updated by readConfigFromFile
        val localSpeed = moe.fuqiuluo.xposed.utils.FakeLoc.speed
        val localHeading = moe.fuqiuluo.xposed.utils.FakeLoc.bearing
        
        val isMoving = localSpeed > 0.5
        if (isMoving != wasMoving) {
            // State Changed. Commit steps from previous state.
            val durationInPrevState = now - lastStateChangeTime
            val prevIsMoving = wasMoving
            val prevDuration = if (prevIsMoving) walkingDuration else idleDuration
            val prevTrack = if (prevIsMoving) walkingTracks[19] else idleTracks[19]
            
            if (prevDuration > 0 && prevTrack != null && prevTrack.frames.isNotEmpty()) {
                val loops = durationInPrevState / prevDuration
                val remainder = durationInPrevState % prevDuration
                val trackTotal = prevTrack.frames.last().values[0] - prevTrack.frames.first().values[0]
                val currentInTrack = (prevTrack.getInterpolatedFrame(remainder)?.get(0) ?: prevTrack.frames.first().values[0]) - prevTrack.frames.first().values[0]
                
                accumulatedSteps += (loops * trackTotal) + currentInTrack
            }
            
            lastStateChangeTime = now
            wasMoving = isMoving
        }

        val loopDuration = if (isMoving) walkingDuration else idleDuration
        val tracks = if (isMoving) walkingTracks else idleTracks
        
        if (loopDuration == 0L || !tracks.containsKey(sensorType)) {
            // Fallback for missing sensors: Return empty to allow original values to pass through
            return FloatArray(0) 
        }

        val timeSinceStateStart = now - lastStateChangeTime
        val pointer = timeSinceStateStart % loopDuration

        // Special handling for Step Counter (19) to be cumulative
        if (sensorType == 19) {
            val track = tracks[19]!!
            if (track.frames.isEmpty()) return floatArrayOf(accumulatedSteps)

            val trackStartVal = track.frames.first().values[0]
            val trackEndVal = track.frames.last().values[0]
            val trackTotal = trackEndVal - trackStartVal
            
            val valInLoop = (track.getInterpolatedFrame(pointer)?.get(0) ?: trackStartVal)
            val deltaInLoop = valInLoop - trackStartVal
            
            val loops = timeSinceStateStart / loopDuration
            
            val currentTotal = accumulatedSteps + (loops * trackTotal) + deltaInLoop
            return floatArrayOf(currentTotal)
        }

        val originalValues = tracks[sensorType]?.getInterpolatedFrame(pointer) ?: FloatArray(3)

        if (isVectorSensor(sensorType)) {
            // Rotation:
            // Target = currentHeading
            // Source = WALKING_BASE_HEADING (180)
            // Delta = Target - Source.
            // Coord System: North=+Y, East=+X. (Phone Flat)
            // Azimuth: 0=N, 90=E. (CW).
            // To go South(180) -> East(90): -90 deg change on compass.
            // In Carthesian: -Y -> +X. +90 deg change (CCW).
            // So theta = -delta.
            // So theta = -delta.
            val deltaDegrees = localHeading - WALKING_BASE_HEADING
            val thetaRad = Math.toRadians(-deltaDegrees)
            
            // Anti-Pattern: Amplitude Modulation
            // Vary the magnitude slightly over time to prevent exact hash matching of the loop.
            // Period: 10s (10000ms). Scale: 0.98 to 1.02 (+/- 2%).
            // Use sin wave for smoothness.
            val modPhase = (now % 10000) / 10000.0 * 2 * Math.PI
            val amplitudeScale = 1.0 + (0.02 * sin(modPhase))
            
            val rotated = rotateVector(originalValues, thetaRad)
            
            // Apply scale
            for (i in rotated.indices) {
                rotated[i] = (rotated[i] * amplitudeScale).toFloat()
            }
            return rotated
        }

        return originalValues
    }

    private fun isVectorSensor(type: Int): Boolean {
        return type == Sensor.TYPE_ACCELEROMETER || 
               type == Sensor.TYPE_GYROSCOPE || 
               type == Sensor.TYPE_MAGNETIC_FIELD ||
               type == Sensor.TYPE_LINEAR_ACCELERATION ||
               type == Sensor.TYPE_GRAVITY ||
               type == Sensor.TYPE_ROTATION_VECTOR // Rotation vector also needs rotation! But it's a quaternion/array. Complex.
               // For now, let's stick to raw vectors. Rotation Vector (11/15) is derived. 
               // Ideally we output raw sensors and let Android derive RV.
               // But if we hook RV, we need to rotate it too.
               // Let's exclude RV for now or just rotate x/y components which corresponds to heading change in RV usually.
    }

    private fun rotateVector(values: FloatArray, thetaRad: Double): FloatArray {
        if (values.size < 3) return values
        // x' = x cos - y sin
        // y' = x sin + y cos
        // Coordinates: x=Right, y=Up (Forward)
        
        val x = values[0]
        val y = values[1]
        val z = values[2] // Z is untouched (gravity axis)

        val c = cos(thetaRad).toFloat()
        val s = sin(thetaRad).toFloat()

        val newX = x * c - y * s
        val newY = x * s + y * c
        
        // Preserve other values if present (e.g. accuracy? no values is float array)
        val result = values.clone()
        result[0] = newX
        result[1] = newY
        // result[2] = z // already there
        
        return result
    }
}
