# ESP32-S3 LLM 管理后台 — 部署镜像
# 后台是纯 Python 标准库实现，无第三方依赖，所以不需要 pip install。
FROM python:3.12-slim

WORKDIR /app

# 让启动/错误日志实时进 docker logs，不被 Python stdout 缓冲吞掉
ENV PYTHONUNBUFFERED=1

# 只拷贝后台代码包；data/ 通过卷挂载持久化，不打进镜像
COPY llm_admin/ /app/llm_admin/

EXPOSE 8766

CMD ["python", "-m", "llm_admin.server", "--host", "0.0.0.0", "--port", "8766"]
