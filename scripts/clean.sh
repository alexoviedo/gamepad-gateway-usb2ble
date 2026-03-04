#!/usr/bin/env bash
set -euo pipefail

rm -rf build sdkconfig sdkconfig.old sdkconfig.*.old managed_components .idf-component-manager
echo "Cleaned generated files. Next build will reconfigure and re-download managed components." 
