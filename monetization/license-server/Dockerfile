FROM node:22-alpine
ENV NODE_ENV=production
WORKDIR /app
COPY --chown=node:node server.mjs licenses.mjs store.mjs stripe.mjs package.json ./
USER node
EXPOSE 8788
CMD ["node", "server.mjs"]
