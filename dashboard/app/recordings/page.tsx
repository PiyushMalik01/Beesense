"use client";

import { useCallback } from "react";
import { RecordingsTable } from "@/components/recordings-table";
import { fetchFiles } from "@/lib/api";
import { usePolling } from "@/lib/use-polling";

export default function RecordingsPage() {
  const filesFetcher = useCallback(() => fetchFiles(), []);
  const { data: files, loading } = usePolling(filesFetcher, 30000);

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-2xl font-bold tracking-tight">Recordings</h1>
        <p className="text-sm text-muted-foreground">
          Browse and play all captured audio recordings
        </p>
      </div>

      <RecordingsTable files={files ?? []} loading={loading} />
    </div>
  );
}
