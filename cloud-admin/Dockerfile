FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY mqtt_admin.py selective_repeat.py file_reassembler.py \
     transfer_queue.py topology.py metrics_store.py web_server.py ./
COPY static/ static/
COPY benchmarks/ benchmarks/

RUN mkdir -p received_files

EXPOSE 5050

ENTRYPOINT ["python3", "mqtt_admin.py"]
CMD ["--broker", "mosquitto", "--port", "1883"]
