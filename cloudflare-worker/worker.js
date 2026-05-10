// ============================================
//  BeeSense Cloud API — Cloudflare Worker + R2
//  Receives WAV uploads and sensor data from ESP32
//  Serves data to the dashboard
// ============================================

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    const corsHeaders = {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, PUT, POST, DELETE, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type, Authorization",
    };

    if (request.method === "OPTIONS") {
      return new Response(null, { headers: corsHeaders });
    }

    // Auth: require API key for uploads (PUT/POST)
    if (request.method === "PUT" || request.method === "POST") {
      const auth = request.headers.get("Authorization");
      if (auth !== `Bearer ${env.API_KEY}`) {
        return json({ error: "Unauthorized" }, 401, corsHeaders);
      }
    }

    try {
      // PUT /api/upload/<filename> — store WAV file in R2
      if (path.startsWith("/api/upload/") && request.method === "PUT") {
        const filename = decodeURIComponent(path.replace("/api/upload/", ""));
        const deviceId = request.headers.get("X-Device-Id") || "unknown";

        await env.BUCKET.put(`wav/${deviceId}/${filename}`, request.body, {
          httpMetadata: { contentType: "audio/wav" },
          customMetadata: { deviceId, uploadedAt: new Date().toISOString() },
        });

        return json({ ok: true, file: filename, path: `wav/${deviceId}/${filename}` }, 201, corsHeaders);
      }

      // POST /api/sensor-data — store sensor reading as JSON in R2
      if (path === "/api/sensor-data" && request.method === "POST") {
        const data = await request.json();
        const ts = (data.timestamp || new Date().toISOString()).replace(/[: ]/g, "_");
        const deviceId = data.device_id || "unknown";
        const key = `data/${deviceId}/${ts}.json`;

        await env.BUCKET.put(key, JSON.stringify({ ...data, stored_at: new Date().toISOString() }), {
          httpMetadata: { contentType: "application/json" },
        });

        return json({ ok: true, key }, 201, corsHeaders);
      }

      // GET /api/files?device=<id> — list WAV files
      if (path === "/api/files" && request.method === "GET") {
        const deviceId = url.searchParams.get("device") || "beesense-01";
        const list = await env.BUCKET.list({ prefix: `wav/${deviceId}/`, limit: 100 });

        const files = list.objects.map((obj) => ({
          name: obj.key.split("/").pop(),
          key: obj.key,
          size: obj.size,
          uploaded: obj.uploaded,
        }));

        return json(files, 200, corsHeaders);
      }

      // GET /api/sensor-data?device=<id>&limit=<n> — list sensor readings
      if (path === "/api/sensor-data" && request.method === "GET") {
        const deviceId = url.searchParams.get("device") || "beesense-01";
        const limit = parseInt(url.searchParams.get("limit") || "50");
        const list = await env.BUCKET.list({ prefix: `data/${deviceId}/`, limit: 1000 });

        const recent = list.objects.slice(-limit);
        const readings = [];
        for (const obj of recent) {
          const data = await env.BUCKET.get(obj.key);
          if (data) readings.push(JSON.parse(await data.text()));
        }

        return json(readings, 200, corsHeaders);
      }

      // GET /api/download/<key> — download a file from R2
      if (path.startsWith("/api/download/") && request.method === "GET") {
        const key = decodeURIComponent(path.replace("/api/download/", ""));
        const file = await env.BUCKET.get(key);
        if (!file) return json({ error: "Not found" }, 404, corsHeaders);

        return new Response(file.body, {
          headers: {
            "Content-Type": file.httpMetadata?.contentType || "application/octet-stream",
            "Content-Disposition": `attachment; filename="${key.split("/").pop()}"`,
            ...corsHeaders,
          },
        });
      }

      // GET /api/export/csv?device=<id> — export ALL sensor data as CSV
      if (path === "/api/export/csv" && request.method === "GET") {
        const deviceId = url.searchParams.get("device") || "beesense-01";
        const allReadings = await getAllSensorData(env, deviceId);

        const header = "timestamp,temperature,humidity,wav_file,sample_rate,device_id,stored_at";
        const rows = allReadings.map((r) =>
          [r.timestamp, r.temperature, r.humidity, r.wav_file, r.sample_rate, r.device_id, r.stored_at || ""].join(",")
        );
        const csv = [header, ...rows].join("\n");

        return new Response(csv, {
          status: 200,
          headers: {
            "Content-Type": "text/csv",
            "Content-Disposition": `attachment; filename="beesense_${deviceId}_data.csv"`,
            ...corsHeaders,
          },
        });
      }

      // GET /api/export/json?device=<id> — export ALL sensor data as JSON
      if (path === "/api/export/json" && request.method === "GET") {
        const deviceId = url.searchParams.get("device") || "beesense-01";
        const allReadings = await getAllSensorData(env, deviceId);

        return new Response(JSON.stringify(allReadings, null, 2), {
          status: 200,
          headers: {
            "Content-Type": "application/json",
            "Content-Disposition": `attachment; filename="beesense_${deviceId}_data.json"`,
            ...corsHeaders,
          },
        });
      }

      // GET /api/files/all?device=<id> — list ALL WAV files (paginated R2 listing)
      if (path === "/api/files/all" && request.method === "GET") {
        const deviceId = url.searchParams.get("device") || "beesense-01";
        const allFiles = [];
        let cursor;

        do {
          const opts = { prefix: `wav/${deviceId}/`, limit: 1000 };
          if (cursor) opts.cursor = cursor;
          const list = await env.BUCKET.list(opts);
          for (const obj of list.objects) {
            allFiles.push({
              name: obj.key.split("/").pop(),
              key: obj.key,
              size: obj.size,
              uploaded: obj.uploaded,
            });
          }
          cursor = list.truncated ? list.cursor : null;
        } while (cursor);

        return json(allFiles, 200, corsHeaders);
      }

      // GET /api/health — check if API is up (for dashboard)
      if (path === "/api/health") {
        return json({ ok: true, service: "beesense-api", time: new Date().toISOString() }, 200, corsHeaders);
      }

      // GET / — API info
      if (path === "/") {
        return json({
          service: "BeeSense Cloud API",
          endpoints: {
            "PUT /api/upload/:filename": "Upload WAV file",
            "POST /api/sensor-data": "Store sensor reading",
            "GET /api/files?device=:id": "List WAV files (recent 100)",
            "GET /api/files/all?device=:id": "List ALL WAV files",
            "GET /api/sensor-data?device=:id&limit=:n": "Get sensor readings",
            "GET /api/export/csv?device=:id": "Export all sensor data as CSV",
            "GET /api/export/json?device=:id": "Export all sensor data as JSON",
            "GET /api/download/:key": "Download a file",
            "GET /api/health": "Health check",
          },
        }, 200, corsHeaders);
      }

      return json({ error: "Not found" }, 404, corsHeaders);
    } catch (err) {
      return json({ error: err.message }, 500, corsHeaders);
    }
  },
};

function json(data, status, corsHeaders) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: { "Content-Type": "application/json", ...corsHeaders },
  });
}

async function getAllSensorData(env, deviceId) {
  const allReadings = [];
  let cursor;

  do {
    const opts = { prefix: `data/${deviceId}/`, limit: 1000 };
    if (cursor) opts.cursor = cursor;
    const list = await env.BUCKET.list(opts);

    for (const obj of list.objects) {
      const data = await env.BUCKET.get(obj.key);
      if (data) {
        try {
          allReadings.push(JSON.parse(await data.text()));
        } catch {
          // skip malformed entries
        }
      }
    }
    cursor = list.truncated ? list.cursor : null;
  } while (cursor);

  allReadings.sort((a, b) => {
    const da = new Date(a.timestamp?.replace(" ", "T") || 0);
    const db = new Date(b.timestamp?.replace(" ", "T") || 0);
    return da - db;
  });

  return allReadings;
}
