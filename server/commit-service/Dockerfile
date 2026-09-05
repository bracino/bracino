# Bracino commit service (DN005) — issue 015 stage A deploy.
FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY commit_service.py fake_publisher.py ./
# runs as uid 1000 by default in this image? No — pin it to match the
# host-side chown of the JSONL bind mount (Stage B runbook).
USER 1000:1000
ENV PYTHONUNBUFFERED=1
CMD ["python", "commit_service.py"]
