from __future__ import annotations

import os

from deerflow.config.app_config import AppConfig
from deerflow.models.factory import create_chat_model
from langchain_core.language_models.chat_models import BaseChatModel

from ssv_agent.config import SsvConfig
from ssv_agent.review.processor import ReviewModelRuntime


def create_review_model(app_config: AppConfig, model_name: str) -> BaseChatModel:
    """SSV 唯一允许直接导入 DeerFlow 的业务边界。"""
    if app_config.tools:
        raise ValueError("复验模型配置不得暴露工具")
    return create_chat_model(model_name, app_config=app_config, attach_tracing=False)


def build_review_model(config: SsvConfig) -> ReviewModelRuntime:
    """通过 DeerFlow 原始 factory 创建配置选定的视觉模型。"""
    selected = next(model for model in config.agent.models if model.name == config.agent.review_model)
    model_config = selected.model_dump(mode="python")
    for key, value in model_config.items():
        if isinstance(value, str) and value.startswith("$"):
            environment_name = value[1:]
            resolved = os.getenv(environment_name)
            if not resolved:
                raise ValueError(f"模型配置环境变量未设置: {environment_name}")
            model_config[key] = resolved
    app_config = AppConfig.model_validate(
        {
            "models": [model_config],
            "sandbox": {"use": "deerflow.sandbox.local:LocalSandboxProvider"},
            "tools": [],
        }
    )
    model = create_review_model(app_config, selected.name)
    return ReviewModelRuntime(model=model, provider="mock" if "mock_provider" in selected.use else "openai_compatible", model_name=selected.model)
