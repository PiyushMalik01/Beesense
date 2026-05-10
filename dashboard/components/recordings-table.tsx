"use client";

import { useState } from "react";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";
import { Skeleton } from "@/components/ui/skeleton";
import { type WAVFile, getDownloadUrl, formatBytes } from "@/lib/api";

interface RecordingsTableProps {
  files: WAVFile[];
  loading: boolean;
  limit?: number;
}

export function RecordingsTable({ files, loading, limit }: RecordingsTableProps) {
  const [playing, setPlaying] = useState<string | null>(null);
  const [audio, setAudio] = useState<HTMLAudioElement | null>(null);

  const sorted = [...files].sort(
    (a, b) => new Date(b.uploaded).getTime() - new Date(a.uploaded).getTime()
  );
  const display = limit ? sorted.slice(0, limit) : sorted;

  function handlePlay(file: WAVFile) {
    if (playing === file.key) {
      audio?.pause();
      setPlaying(null);
      setAudio(null);
      return;
    }

    audio?.pause();
    const a = new Audio(getDownloadUrl(file.key));
    a.play();
    a.onended = () => {
      setPlaying(null);
      setAudio(null);
    };
    setPlaying(file.key);
    setAudio(a);
  }

  function formatDate(dateStr: string) {
    const d = new Date(dateStr);
    if (isNaN(d.getTime())) return dateStr;
    return d.toLocaleString("en-IN", {
      day: "2-digit",
      month: "short",
      year: "numeric",
      hour: "2-digit",
      minute: "2-digit",
      hour12: true,
    });
  }

  return (
    <Card>
      <CardHeader>
        <CardTitle className="text-base">
          {limit ? "Recent Recordings" : `All Recordings (${files.length})`}
        </CardTitle>
      </CardHeader>
      <CardContent>
        {loading ? (
          <div className="space-y-3">
            {Array.from({ length: limit ?? 5 }).map((_, i) => (
              <Skeleton key={i} className="h-10 w-full" />
            ))}
          </div>
        ) : display.length === 0 ? (
          <p className="py-8 text-center text-muted-foreground">
            No recordings yet
          </p>
        ) : (
          <div className="overflow-x-auto">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>Filename</TableHead>
                  <TableHead>Date</TableHead>
                  <TableHead className="text-right">Size</TableHead>
                  <TableHead className="text-right">Actions</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {display.map((file) => (
                  <TableRow key={file.key}>
                    <TableCell className="font-mono text-sm">
                      {file.name}
                    </TableCell>
                    <TableCell className="text-sm text-muted-foreground">
                      {formatDate(file.uploaded)}
                    </TableCell>
                    <TableCell className="text-right text-sm">
                      {formatBytes(file.size)}
                    </TableCell>
                    <TableCell className="text-right">
                      <div className="flex items-center justify-end gap-2">
                        <Button
                          variant="ghost"
                          size="sm"
                          onClick={() => handlePlay(file)}
                          className="h-8 px-2"
                        >
                          {playing === file.key ? "⏹ Stop" : "▶ Play"}
                        </Button>
                        <a
                          href={getDownloadUrl(file.key)}
                          download={file.name}
                          target="_blank"
                          rel="noopener noreferrer"
                          className="inline-flex h-8 items-center rounded-md px-2 text-sm font-medium hover:bg-accent hover:text-accent-foreground"
                        >
                          ↓ Download
                        </a>
                      </div>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </div>
        )}
      </CardContent>
    </Card>
  );
}
