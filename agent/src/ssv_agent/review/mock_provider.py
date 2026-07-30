from __future__ import annotations

from langchain_core.language_models.chat_models import BaseChatModel
from langchain_core.messages import AIMessage, BaseMessage
from langchain_core.outputs import ChatGeneration, ChatResult


class MockVisionChatModel(BaseChatModel):
    """无网络视觉模型，用于固定 Demo 自动化闭环。"""

    model: str = "demo-vision"
    response_json: str = (
        '{"decision":"confirmed_no_helmet","review_confidence":0.93,'
        '"primary_reason_code":"no_helmet_visible",'
        '"evidence_summary":"目标头部清晰可见，未观察到安全帽。",'
        '"recommended_action":"生成未佩戴安全帽复验记录。"}'
    )

    @property
    def _llm_type(self) -> str:
        return "ssv_mock_vision"

    def _generate(self, messages: list[BaseMessage], **_: object) -> ChatResult:
        return ChatResult(generations=[ChatGeneration(message=AIMessage(content=self.response_json))])
