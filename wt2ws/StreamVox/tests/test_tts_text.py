from streamvox.tts.text import sanitize_for_tts


def test_markdown_list_and_bold_are_removed():
    raw = "1. **迈克尔·乔丹**——六冠六FMVP\n2. **勒布朗·詹姆斯**——四冠"
    assert sanitize_for_tts(raw) == "迈克尔·乔丹——六冠六FMVP。勒布朗·詹姆斯——四冠"


def test_links_code_and_emoji_are_removed():
    raw = "看这里 [官网](https://example.com) `code` 😀\n### 标题"
    out = sanitize_for_tts(raw)
    assert "http" not in out
    assert "**" not in out
    assert "😀" not in out
    assert "code" in out


def test_plain_text_is_unchanged():
    assert sanitize_for_tts("你好，我是 StreamVox。") == "你好，我是 StreamVox。"
