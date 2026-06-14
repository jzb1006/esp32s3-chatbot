# ESP32-S3 设备网关 — 部署镜像
# 网关是纯 Python 标准库实现，无第三方依赖，所以不需要 pip install。
FROM python:3.12-slim

WORKDIR /app

# 让启动/错误日志实时进 docker logs，不被 Python stdout 缓冲吞掉
ENV PYTHONUNBUFFERED=1

# 只拷贝后台代码包；data/ 通过卷挂载持久化，不打进镜像
COPY device_gateway/ /app/device_gateway/

EXPOSE 8766

CMD ["python", "-m", "device_gateway.server", "--host", "0.0.0.0", "--port", "8766"]
