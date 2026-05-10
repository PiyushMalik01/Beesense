const API_URL = process.env.NEXT_PUBLIC_API_URL ?? "";
const DEVICE_ID = process.env.NEXT_PUBLIC_DEVICE_ID ?? "beesense-01";

export interface SensorReading {
  device_id: string;
  timestamp: string;
  temperature: number;
  humidity: number;
  wav_file: string;
  sample_rate: number;
  stored_at?: string;
}

export interface WAVFile {
  name: string;
  key: string;
  size: number;
  uploaded: string;
}

export interface HealthStatus {
  ok: boolean;
  service: string;
  time: string;
}

async function apiFetch<T>(path: string): Promise<T> {
  const res = await fetch(`${API_URL}${path}`, { cache: "no-store" });
  if (!res.ok) throw new Error(`API error: ${res.status}`);
  return res.json();
}

export function getDeviceId() {
  return DEVICE_ID;
}

export function getApiUrl() {
  return API_URL;
}

export async function fetchHealth(): Promise<HealthStatus> {
  return apiFetch("/api/health");
}

export async function fetchSensorData(
  limit = 50
): Promise<SensorReading[]> {
  return apiFetch(
    `/api/sensor-data?device=${DEVICE_ID}&limit=${limit}`
  );
}

export async function fetchFiles(): Promise<WAVFile[]> {
  return apiFetch(`/api/files?device=${DEVICE_ID}`);
}

export async function fetchAllFiles(): Promise<WAVFile[]> {
  return apiFetch(`/api/files/all?device=${DEVICE_ID}`);
}

export function getDownloadUrl(key: string): string {
  return `${API_URL}/api/download/${encodeURIComponent(key)}`;
}

export function getExportCsvUrl(): string {
  return `${API_URL}/api/export/csv?device=${DEVICE_ID}`;
}

export function getExportJsonUrl(): string {
  return `${API_URL}/api/export/json?device=${DEVICE_ID}`;
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  const kb = bytes / 1024;
  if (kb < 1024) return `${kb.toFixed(0)} KB`;
  return `${(kb / 1024).toFixed(1)} MB`;
}

export function formatTimestamp(ts: string): string {
  const d = new Date(ts.replace(" ", "T"));
  if (isNaN(d.getTime())) return ts;
  return d.toLocaleString("en-IN", {
    day: "2-digit",
    month: "short",
    year: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    hour12: true,
  });
}

export function timeAgo(ts: string): string {
  const d = new Date(ts.replace(" ", "T"));
  if (isNaN(d.getTime())) return "unknown";
  const diff = Date.now() - d.getTime();
  const mins = Math.floor(diff / 60000);
  if (mins < 1) return "just now";
  if (mins < 60) return `${mins}m ago`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}h ago`;
  const days = Math.floor(hours / 24);
  return `${days}d ago`;
}
