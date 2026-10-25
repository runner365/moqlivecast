import asyncio

from streamvox.asr.mock import MockAsr


def test_mock_stream_collects_chunks_and_returns_final_text():
    async def run():
        asr = MockAsr("最终文本", delay_ms=1)
        stream = await asr.open_stream(16000)
        assert stream is not None
        await stream.feed(b"a" * 3200)
        await stream.feed(b"b" * 3200)
        result = await stream.finish()
        assert result.text == "最终文本"
        assert result.raw["streaming"] is True
        assert result.raw["bytes"] == 6400

    asyncio.run(run())


def test_mock_stream_cancel_is_safe():
    async def run():
        asr = MockAsr("x", delay_ms=1)
        stream = await asr.open_stream(16000)
        await stream.feed(b"a" * 100)
        await stream.cancel()
        result = await stream.finish()
        assert result.text == ""

    asyncio.run(run())
