"use client";

import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Skeleton } from "@/components/ui/skeleton";
import { type SensorReading, timeAgo, formatTimestamp, getDeviceId } from "@/lib/api";

interface DeviceStatusProps {
  latestReading: SensorReading | null;
  loading: boolean;
}

export function DeviceStatus({ latestReading, loading }: DeviceStatusProps) {
  const isOnline = (() => {
    if (!latestReading) return false;
    const d = new Date(latestReading.timestamp.replace(" ", "T"));
    if (isNaN(d.getTime())) return false;
    return Date.now() - d.getTime() < 15 * 60 * 1000;
  })();

  return (
    <Card>
      <CardHeader className="pb-3">
        <CardTitle className="flex items-center justify-between text-base">
          Device Status
          {loading ? (
            <Skeleton className="h-5 w-16" />
          ) : (
            <Badge variant={isOnline ? "default" : "destructive"}>
              {isOnline ? "Online" : "Offline"}
            </Badge>
          )}
        </CardTitle>
      </CardHeader>
      <CardContent className="space-y-2 text-sm">
        <Row label="Device ID" value={getDeviceId()} loading={false} />
        <Row
          label="Last Seen"
          value={latestReading ? timeAgo(latestReading.timestamp) : "—"}
          loading={loading}
        />
        <Row
          label="Last Recording"
          value={
            latestReading
              ? formatTimestamp(latestReading.timestamp)
              : "—"
          }
          loading={loading}
        />
        <Row
          label="Sample Rate"
          value={
            latestReading ? `${latestReading.sample_rate} Hz` : "—"
          }
          loading={loading}
        />
      </CardContent>
    </Card>
  );
}

function Row({
  label,
  value,
  loading,
}: {
  label: string;
  value: string;
  loading: boolean;
}) {
  return (
    <div className="flex items-center justify-between">
      <span className="text-muted-foreground">{label}</span>
      {loading ? <Skeleton className="h-4 w-24" /> : <span>{value}</span>}
    </div>
  );
}
