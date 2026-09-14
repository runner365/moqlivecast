# 设计
在本目录用vue3实现一个简单的webtransport客户端工具，chat meeting.主要功能：
* webtransport 连接服务器：https://127.0.0.1:4433/chat?roomid=123456，通过roomid加入会议，制定subpath为/chat
* 发送消息：点击发送按钮，发送消息到服务器, 服务器通过roomid广播消息给roomid内的所有客户端
* 接收消息：服务器通过roomid广播消息给roomid内的所有客户端，客户端收到消息后，显示在消息列表中

服务端：wt_chat_server.c
* 采用http3/webtransport_server_api.h对应的api实现webtransport服务端，实现subpath为/chat的webtransport服务, 支持多个roomid的会议
* 支持roomid的创建和加入, 有roomManager管理多个roomid的会议
* 支持消息的广播和接收, 每个roomid内的客户端收到的消息，广播给其他客户端


