
FROM python:3.11-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

WORKDIR /app


COPY backend/server/ ./server/
COPY common/ ./common/

RUN pip install --no-cache-dir --no-compile -r ./server/requirements.txt

CMD ["python", "-u", "server/arena_server.py"]  
