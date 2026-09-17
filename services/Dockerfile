# One image for every service in this workspace.
#
# The API and the feeds share all their dependencies and their build step, so
# building them separately would mean running the same npm install and tsc
# several times over for no gain. Compose picks which one to run with `command`.
FROM node:22-alpine AS build
WORKDIR /app
COPY package.json package-lock.json tsconfig.base.json tsconfig.json ./
COPY packages ./packages
COPY api ./api
COPY feeds ./feeds
RUN npm ci
RUN npm run build

FROM node:22-alpine
WORKDIR /app
ENV NODE_ENV=production
COPY package.json package-lock.json ./
COPY packages ./packages
COPY api ./api
COPY feeds ./feeds
# Drops the dev dependencies (typescript, vitest) from the runtime image; the
# compiled dist/ directories come from the build stage below.
RUN npm ci --omit=dev
COPY --from=build /app/packages ./packages
COPY --from=build /app/api ./api
COPY --from=build /app/feeds ./feeds
CMD ["node", "api/dist/index.js"]
