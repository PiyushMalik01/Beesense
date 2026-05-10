"use client";

import { useCallback } from "react";
import { StatCard } from "@/components/stat-card";
import { DeviceStatus } from "@/components/device-status";
import { SensorCharts } from "@/components/sensor-charts";
import { RecordingsTable } from "@/components/recordings-table";
import { fetchSensorData, fetchFiles } from "@/lib/api";
import { usePolling } from "@/lib/use-polling";

export default function OverviewPage() {
  const sensorFetcher = useCallback(() => fetchSensorData(100), []);
  const filesFetcher = useCallback(() => fetchFiles(), []);

  const { data: readings, loading: rl } = usePolling(sensorFetcher, 30000);
  const { data: files, loading: fl } = usePolling(filesFetcher, 30000);

  const latest = readings?.length ? readings[readings.length - 1] : null;

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-2xl font-bold tracking-tight">Overview</h1>
        <p className="text-sm text-muted-foreground">
          Real-time beehive monitoring dashboard
        </p>
      </div>

      <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
        <StatCard
          title="Temperature"
          value={latest ? `${latest.temperature.toFixed(1)}°C` : "—"}
          subtitle="Current reading"
          icon={<ThermometerIcon />}
          loading={rl}
        />
        <StatCard
          title="Humidity"
          value={latest ? `${latest.humidity.toFixed(1)}%` : "—"}
          subtitle="Current reading"
          icon={<DropletIcon />}
          loading={rl}
        />
        <StatCard
          title="Recordings"
          value={files ? String(files.length) : "—"}
          subtitle="Total WAV files"
          icon={<MicIcon />}
          loading={fl}
        />
        <DeviceStatus latestReading={latest} loading={rl} />
      </div>

      <SensorCharts readings={readings ?? []} loading={rl} />

      <RecordingsTable files={files ?? []} loading={fl} limit={5} />
    </div>
  );
}

function ThermometerIcon() {
  return (
    <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M14 4v10.54a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z" />
    </svg>
  );
}

function DropletIcon() {
  return (
    <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15a7 7 0 0 0 7 7z" />
    </svg>
  );
}

function MicIcon() {
  return (
    <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z" />
      <path d="M19 10v2a7 7 0 0 1-14 0v-2" />
      <line x1="12" x2="12" y1="19" y2="22" />
    </svg>
  );
}
