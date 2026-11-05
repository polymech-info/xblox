#!/usr/bin/env bash
set -euo pipefail

npm run test:xblox-next -- --only engine
npm run test:xblox-next -- --only variables
npm run test:xblox-next -- --only wiring
npm run test:xblox-next -- --only expressions
npm run test:xblox-next -- --only loops
npm run test:xblox-next -- --only previous
npm run test:xblox-next -- --only containers
npm run test:xblox-next -- --only scoping
npm run test:xblox-next -- --only transform
npm run test:xblox-next -- --only simulate
npm run test:xblox-next -- --only constraints
npm run test:xblox-next -- --only globs
npm run test:xblox-next -- --only hooks
npm run test:xblox-next -- --only iterate
npm run test:xblox-next -- --only network
npm run test:xblox-next -- --only xbloxRun
npm run test:xblox-next -- --only std
npm run test:xblox-next -- --only jq
npm run test:xblox-next -- --only fsVars
npm run test:xblox-next -- --only refs
