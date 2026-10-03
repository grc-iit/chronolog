#!/usr/bin/env bash
# ARCHITECTURE.md S14.5: core never logs payload bytes. Scans every absl logging and CHECK
# statement under src/ (to its terminating semicolon, comments and string literals removed)
# for a payload identifier such as envelope.payload, payload() or payload_bytes.
set -euo pipefail
root=$1
find "$root/src" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \) -print0 |
    xargs -0 perl -e '
        undef $/;
        my $fail = 0;
        for my $file (@ARGV) {
            open my $in, "<", $file or die "$file: $!";
            my $text = <$in>;
            $text =~ s{//[^\n]*|/\*.*?\*/}{ }gs;
            while ($text =~ /\b((?:ABSL_)?(?:[DPQ]?LOG|VLOG|[DPQ]?CHECK)(?:_[A-Z0-9_]+)?\s*\(.*?;)/gs) {
                my ($statement, $start) = ($1, $-[1]);
                (my $code = $statement) =~ s/"(?:\\.|[^"\\])*"/""/g;
                next unless $code =~ /payload/i;
                my $line = 1 + (() = substr($text, 0, $start) =~ /\n/g);
                print STDERR "$file:$line: logs payload: $statement\n";
                $fail = 1;
            }
        }
        exit $fail;
    ' || {
    echo 'Logging macro under src/ mentions payload (S14.5)' >&2
    exit 1
}
