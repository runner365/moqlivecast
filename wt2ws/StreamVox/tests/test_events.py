from streamvox import events
from streamvox.realtime.session import pop_sentence


def test_ga_and_beta_names():
    assert events.audio_delta("ga", "r", "i", "AA==")["type"] == "response.output_audio.delta"
    assert events.audio_delta("beta", "r", "i", "AA==")["type"] == "response.audio.delta"
    assert events.text_delta("ga", "r", "i", "hi")["type"] == "response.output_text.delta"
    assert events.text_delta("beta", "r", "i", "hi")["type"] == "response.text.delta"
    assert events.audio_transcript_delta("ga", "r", "i", "hi")["type"] == \
        "response.output_audio_transcript.delta"
    assert events.audio_done("beta", "r", "i")["type"] == "response.audio.done"


def test_session_object_has_flat_and_nested_formats():
    session = events.session_object("sess_1", "be nice", "voice_a", "asr_a", 24000)
    assert session["input_audio_format"] == "pcm16"
    assert session["audio"]["input"]["format"]["rate"] == 16000
    assert session["audio"]["output"]["format"]["rate"] == 24000
    assert session["audio"]["output"]["voice"] == "voice_a"


def test_sentence_splitter():
    sentence, rest = pop_sentence("你好。再见")
    assert sentence == "你好。"
    assert rest == "再见"

    sentence, rest = pop_sentence("This is a complete sentence. Next")
    assert sentence == "This is a complete sentence. "
    assert rest == "Next"

    sentence, rest = pop_sentence("too short. ")
    assert sentence is None
    assert rest == "too short. "
