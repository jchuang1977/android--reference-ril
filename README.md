### 1. **简介** 

    reference-ril ,是蜂窝模组对安卓的一个适配层，通过不同的refrence ril ,可以让同一个安卓系统使用不同的厂家的蜂窝模组，本着开源的精神，我司将代码开源。
### 2. **框架图**

     

### 3. **原理**

    reference-RIL 主要负责：
    
    1、将安卓的 Request请求转化成AT命令交给  Modem 执行，并将AT命令执行的结果以Response消息的方式反馈给 安卓;
    
    2、接收Modem的主动上报的消息，以UnSolicited Response消息的方式反馈给安卓的 LibRIL处理
    
### 4. **驱动配置**

    https://e3zt58hesn.feishu.cn/docx/ZjnGdKT13oj16dxpRtCcPfHknRb