FROM node:22-slim AS build
WORKDIR /app
RUN apt-get update && apt-get install -y git && rm -rf /var/lib/apt/lists/*
COPY package.json package-lock.json ./
RUN npm ci
COPY . .
RUN npm run build

FROM node:22-slim AS runtime
WORKDIR /app
ENV NODE_ENV=production

COPY --from=build /app/dist ./dist
COPY --from=build /app/server ./server
COPY --from=build /app/mock ./mock
COPY --from=build /app/node_modules ./node_modules

EXPOSE 8085
CMD ["node", "server/unified-server.mjs"]
