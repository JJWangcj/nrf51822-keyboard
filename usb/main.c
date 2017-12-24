#include "CH554_SDCC.h"
#include "compiler.h"
#include "Debug.H"
#include "usb_descriptor.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#define LED RXD1

#define THIS_ENDP0_SIZE         DEFAULT_ENDP0_SIZE

uint8_t __xdata Ep0Buffer[THIS_ENDP0_SIZE+2 >= MAX_PACKET_SIZE ? MAX_PACKET_SIZE : THIS_ENDP0_SIZE+2];    //端点0 OUT&IN缓冲区，必须是偶地址
uint8_t __xdata Ep1Buffer[MAX_PACKET_SIZE];  //端点1 IN缓冲区,必须是偶地址
uint8_t __xdata Ep2Buffer[2*MAX_PACKET_SIZE];  //端点2 IN缓冲区,必须是偶地址
uint8_t   SetupReq,SetupLen,Ready,Count,FLAG,UsbConfig;
uint8_t *pDescr;                                                                //USB配置标志
USB_SETUP_REQ   SetupReqBuf;                                                   //暂存Setup包
#define Ep2InKey P1_5;

#define UsbSetupBuf     ((PUSB_SETUP_REQ)Ep0Buffer)
/*键盘数据*/
uint8_t HIDKey[8] = {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0};
uint8_t HIDMouse[8] = {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0};

void printHEX(uint8_t * string, uint8_t len)
{
    uint8_t i;
    for( i=0;i<len;i++)
    {
        printf("%02x ", string[i]);
    }

}


/** \brief CH554软复位
 *
 */
void CH554SoftReset()
{
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG	|=bSW_RESET;
}

/** \brief CH554设备模式唤醒主机，发送K信号
 *
 */
void CH554USBDevWakeup( )
{
    UDEV_CTRL |= bUD_LOW_SPEED;
    mDelaymS(2);
    UDEV_CTRL &= ~bUD_LOW_SPEED;
}


/** \brief USB设备模式配置,设备模式启动，收发端点配置，中断开启
 *
 */
void USBDeviceInit()
{
    IE_USB = 0;
    USB_CTRL = 0x00;                                                           // 先设定USB设备模式

    UEP0_DMA = (uint16_t)Ep0Buffer;                                            //端点0数据传输地址
    UEP4_1_MOD &= ~(bUEP4_RX_EN | bUEP4_TX_EN);                                //端点0单64字节收发缓冲区
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;                                 //OUT事务返回ACK，IN事务返回NAK

    UEP1_DMA = (uint16_t)Ep1Buffer;                                            //端点1数据传输地址
    UEP4_1_MOD = UEP4_1_MOD & ~bUEP1_BUF_MOD | bUEP1_TX_EN;                    //端点1发送使能 64字节缓冲区
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;                                 //端点1自动翻转同步标志位，IN事务返回NAK

    UEP2_DMA = (uint16_t)Ep2Buffer;                                            //端点2数据传输地址
    UEP2_3_MOD = UEP2_3_MOD & ~bUEP2_BUF_MOD | bUEP2_TX_EN;                    //端点2发送使能 64字节缓冲区
    UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;                                 //端点2自动翻转同步标志位，IN事务返回NAK

    USB_DEV_AD = 0x00;
    UDEV_CTRL = bUD_PD_DIS;                                                    // 禁止DP/DM下拉电阻
    USB_CTRL = bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;                      // 启动USB设备及DMA，在中断期间中断标志未清除前自动返回NAK
    UDEV_CTRL |= bUD_PORT_EN;                                                  // 允许USB端口
    USB_INT_FG = 0xFF;                                                         // 清中断标志
    USB_INT_EN = bUIE_SUSPEND | bUIE_TRANSFER | bUIE_BUS_RST;
    IE_USB = 1;
}


/** \brief USB设备模式端点1的中断上传
 *
 */
void Enp1IntIn( )
{
    memcpy( Ep1Buffer, HIDKey, sizeof(HIDKey));                              //加载上传数据
    UEP1_T_LEN = sizeof(HIDKey);                                             //上传数据长度
    UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;                //有数据时上传数据并应答ACK
}

/** \brief USB设备模式端点2的中断上传
*/
void Enp2IntIn( )
{
    memcpy( Ep2Buffer, HIDMouse, sizeof(HIDMouse));                           //加载上传数据
    UEP2_T_LEN = sizeof(HIDMouse);                                            //上传数据长度
    UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;                 //有数据时上传数据并应答ACK
}


/** \brief 获取文本描述符
 *
 * \param order uint8_t 文本描述符顺序
 * \param strPtor uint8_t* 输出指针
 * \return uint8_t 长度
 *
 */
uint8_t getStringDescriptor(uint8_t order, uint8_t ** strPtor)
{
    uint8_t header = 0, strlen = 0;

    printf_tiny("desc: %d ptr:", order);
    do
    {
        header += strlen;
        if(header >= sizeof(StringDescriptor)) // 超过长度就直接返回
        {
            return 0xFF;
        }
        strlen = StringDescriptor[header];
    }
    while(order--);

    *strPtor = (uint8_t *)&StringDescriptor[header];
    printHEX(*strPtor, strlen);
    return strlen;
}

/** \brief CH559USB中断处理函数
 */
void DeviceInterrupt( void ) __interrupt INT_NO_USB __using 1                   //USB中断服务程序,使用寄存器组1
{
    uint8_t len = 0;

    if(UIF_TRANSFER)                                                            //USB传输完成标志
    {
        switch (USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
        {
            case UIS_TOKEN_IN | 2:                                                  //endpoint 2# 中断端点上传
                UEP2_T_LEN = 0;                                                     //预使用发送长度一定要清空
//              UEP1_CTRL ^= bUEP_T_TOG;                                            //如果不设置自动翻转则需要手动翻转
                UEP2_CTRL = UEP2_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK;           //默认应答NAK
                break;
            case UIS_TOKEN_IN | 1:                                                  //endpoint 1# 中断端点上传
                UEP1_T_LEN = 0;                                                     //预使用发送长度一定要清空
//              UEP2_CTRL ^= bUEP_T_TOG;                                            //如果不设置自动翻转则需要手动翻转
                UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK;           //默认应答NAK
                FLAG = 1;                                                           //传输完成标志
                break;
            case UIS_TOKEN_SETUP | 0:                                               //SETUP事务
                len = USB_RX_LEN;
                if(len == (sizeof(USB_SETUP_REQ)))
                {
                    SetupLen = UsbSetupBuf->wLengthL;
                    if(UsbSetupBuf->wLengthH || SetupLen > 0x7F )
                    {
                        SetupLen = 0x7F;    // 限制总长度
                    }
                    len = 0;                                                        // 默认为成功并且上传0长度
                    SetupReq = UsbSetupBuf->bRequest;
                    if ( ( UsbSetupBuf->bRequestType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )/* HID类命令 */
                    {
                        switch( SetupReq )
                        {
                            case 0x01://GetReport
                                break;
                            case 0x02://GetIdle
                                break;
                            case 0x03://GetProtocol
                                break;
                            case 0x09://SetReport
                                break;
                            case 0x0A://SetIdle
                                break;
                            case 0x0B://SetProtocol
                                break;
                            default:
                                len = 0xFF;  								 					            /*命令不支持*/
                                break;
                        }
                    }
                    else
                    {
                        //标准请求
                        switch(SetupReq)                                        //请求码
                        {
                            case USB_GET_DESCRIPTOR:
                                switch(UsbSetupBuf->wValueH)
                                {
                                    case 1:                                             // 设备描述符
                                        pDescr = (uint8_t *)&DeviceDescriptor[0];                               //把设备描述符送到要发送的缓冲区
                                        len = sizeof(DeviceDescriptor);
                                        break;
                                    case 2:                                             // 配置描述符
                                        pDescr = (uint8_t *)&ConfigDescriptor[0];                               //把设备描述符送到要发送的缓冲区
                                        len = sizeof(ConfigDescriptor);
                                        break;
                                    case 3:                                             // 字符串描述符
                                        len = getStringDescriptor(UsbSetupBuf->wValueL, &pDescr);
                                        break;
                                    case 0x22:                                          //报表描述符
                                        if(UsbSetupBuf->wIndexL == 0)                   //接口0报表描述符
                                        {
                                            pDescr = (uint8_t *)&report_desc_HID0[0];                        //数据准备上传
                                            len = sizeof(report_desc_HID0);
                                        }
                                        else if(UsbSetupBuf->wIndexL == 1)              //接口1报表描述符
                                        {
                                            pDescr = (uint8_t *)&report_desc_HID1[0];                      //数据准备上传
                                            len = sizeof(report_desc_HID1);
                                            Ready = 1;                                  //如果有更多接口，该标准位应该在最后一个接口配置完成后有效
                                        }
                                        else
                                        {
                                            len = 0xff;                                 //本程序只有2个接口，这句话正常不可能执行
                                        }
                                        break;
                                    default:
                                        len = 0xff;                                     //不支持的命令或者出错
                                        break;
                                }
                                if ( SetupLen > len )
                                {
                                    SetupLen = len;    //限制总长度
                                }
                                len = SetupLen >= THIS_ENDP0_SIZE ? THIS_ENDP0_SIZE : SetupLen;                  //本次传输长度
                                memcpy(Ep0Buffer,pDescr,len);                        //加载上传数据
                                SetupLen -= len;
                                pDescr += len;
                                break;

                            case USB_SET_ADDRESS:
                                SetupLen = UsbSetupBuf->wValueL;                     //暂存USB设备地址
                                break;

                            case USB_GET_CONFIGURATION:
                                Ep0Buffer[0] = UsbConfig;
                                if ( SetupLen >= 1 ) len = 1;
                                break;

                            case USB_SET_CONFIGURATION:
                                UsbConfig = UsbSetupBuf->wValueL;
                                break;

                            case USB_GET_INTERFACE:
                                Ep0Buffer[0] = 0x00;
								if ( SetupLen >= 1 ) len = 1;
                                break;

                            case USB_CLEAR_FEATURE:                                            //Clear Feature
                                if ( ( UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )// 端点
                                {
                                    switch( UsbSetupBuf->wIndexL )
                                    {
                                        case 0x82:
                                            UEP2_CTRL = UEP2_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                            break;
                                        case 0x81:
                                            UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                            break;
                                        case 0x01:
                                            UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
                                            break;
                                        default:
                                            len = 0xFF;                                            // 不支持的端点
                                            break;
                                    }
                                }
                                if ( ( UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )// 设备
                                {
                                    break;
                                }
                                else
                                {
                                    len = 0xFF;                                                // 不是端点不支持
                                }
                                break;
                            case USB_SET_FEATURE:                                              /* Set Feature */
                                if( ( UsbSetupBuf->bRequestType & 0x1F ) == 0x00 )             /* 设置设备 */
                                {
                                    if( ( ( ( uint16_t )UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x01 )
                                    {
                                        if( ConfigDescriptor[ 7 ] & 0x20 )
                                        {
                                            /* 设置唤醒使能标志 */
                                        }
                                        else
                                        {
                                            len = 0xFF;                                        /* 操作失败 */
                                        }
                                    }
                                    else
                                    {
                                        len = 0xFF;                                            /* 操作失败 */
                                    }
                                }
                                else if( ( UsbSetupBuf->bRequestType & 0x1F ) == 0x02 )        /* 设置端点 */
                                {
                                    if((((uint16_t)UsbSetupBuf->wValueH << 8 ) | UsbSetupBuf->wValueL ) == 0x00 )
                                    {
                                        switch( ( ( uint16_t )UsbSetupBuf->wIndexH << 8 ) | UsbSetupBuf->wIndexL )
                                        {
                                            case 0x82:
                                                UEP2_CTRL = UEP2_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;/* 设置端点2 IN STALL */
                                                break;
                                            case 0x02:
                                                UEP2_CTRL = UEP2_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL;/* 设置端点2 OUT Stall */
                                                break;
                                            case 0x81:
                                                UEP1_CTRL = UEP1_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;/* 设置端点1 IN STALL */
                                                break;
                                            default:
                                                len = 0xFF;                               //操作失败
                                                break;
                                        }
                                    }
                                    else
                                    {
                                        len = 0xFF;                                   //操作失败
                                    }
                                }
                                else
                                {
                                    len = 0xFF;                                      //操作失败
                                }
                                break;
                            case USB_GET_STATUS:
                                Ep0Buffer[0] = 0x00;
                                Ep0Buffer[1] = 0x00;
                                if ( SetupLen >= 2 )
                                {
                                    len = 2;
                                }
                                else
                                {
                                    len = SetupLen;
                                }
                                break;
                            default:
                                len = 0xff;                                           //操作失败
                                break;
                        }
                    }
                }
                else
                {
                    len = 0xff;                                                   //包长度错误
                }
                if(len == 0xff)
                {
                    SetupReq = 0xFF;
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;//STALL
                }
                else if(len <= DEFAULT_ENDP0_SIZE)                                                //上传数据或者状态阶段返回0长度包
                {
                    UEP0_T_LEN = len;
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;//默认数据包是DATA1，返回应答ACK
                }
                else
                {
                    UEP0_T_LEN = 0;  //虽然尚未到状态阶段，但是提前预置上传0长度数据包以防主机提前进入状态阶段
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;//默认数据包是DATA1,返回应答ACK
                }
                break;
            case UIS_TOKEN_IN | 0:                                               //endpoint0 IN
                switch(SetupReq)
                {
                    case USB_GET_DESCRIPTOR:
                        len = SetupLen >= THIS_ENDP0_SIZE ? THIS_ENDP0_SIZE : SetupLen;                          //本次传输长度
                        memcpy( Ep0Buffer, pDescr, len );                            //加载上传数据
                        SetupLen -= len;
                        pDescr += len;
                        UEP0_T_LEN = len;
                        UEP0_CTRL ^= bUEP_T_TOG;                                     //同步标志位翻转
                        break;
                    case USB_SET_ADDRESS:
                        USB_DEV_AD = USB_DEV_AD & bUDA_GP_BIT | SetupLen;
                        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                        break;
                    default:
                        UEP0_T_LEN = 0;                                              //状态阶段完成中断或者是强制上传0长度数据包结束控制传输
                        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                        break;
                }
                break;
            case UIS_TOKEN_OUT | 0:  // endpoint0 OUT
                len = USB_RX_LEN;
                if(SetupReq == 0x09)
                {
                    if(Ep0Buffer[0])
                    {
                        // printf("Light on Num Lock LED!\n");
                    }
                    else if(Ep0Buffer[0] == 0)
                    {
                        // printf("Light off Num Lock LED!\n");
                    }
                }
                UEP0_CTRL ^= bUEP_R_TOG;                                     //同步标志位翻转
                break;
            case USB_GET_DESCRIPTOR:
                UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;           // 准备下一控制传输
                break;
            default:
                break;
        }
        UIF_TRANSFER = 0;                                                 //写0清空中断
    }
    if(UIF_BUS_RST)                                                       //设备模式USB总线复位中断
    {
        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        UEP1_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK;
        UEP2_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
        USB_DEV_AD = 0x00;
        UIF_SUSPEND = 0;
        UIF_TRANSFER = 0;
        UIF_BUS_RST = 0;                                                 //清中断标志
    }
    if (UIF_SUSPEND)                                                     //USB总线挂起/唤醒完成
    {
        UIF_SUSPEND = 0;
        if ( USB_MIS_ST & bUMS_SUSPEND )                                 //挂起
        {
        }
    }
    else                                                                 //意外的中断,不可能发生的情况
    {
        USB_INT_FG = 0xFF;                                               //清中断标志
    }
}
/** \brief 处理各种请求
 *
 */

void CfgSysClock();

void main()
{
    CfgSysClock();
    mDelaymS(5);                                                          //修改主频等待内部晶振稳定,必加
    mInitSTDIO( );                                                        //串口0初始化
    mDelaymS(5);
    printf_tiny("Hello, world1\n");

    USBDeviceInit();                                                      //USB设备模式初始化
    EA = 1;                                                               //允许单片机中断
    UEP1_T_LEN = 0;                                                       //预使用发送长度一定要清空
    UEP2_T_LEN = 0;                                                       //预使用发送长度一定要清空
    FLAG = 0;
    Ready = 0;

    while(1)
    {
        /*
        if(Ready)
        {
            // HIDValueHandle();
        }

        if(Ready && Ep2InKey == 0)
        {
            CH554USBDevWakeup();
            mDelaymS(10);
            Enp1IntIn();
        }
        */
        mDelaymS( 100 );


        LED = !LED;
        mDelaymS(500);
    }
}

/** \brief 配置系统时钟
 *
 * \param void
 * \return void
 *
 */
void	CfgSysClock( )
{
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG = CLOCK_CFG & ~ MASK_SYS_CK_SEL | 0x04;  // 12MHz
    SAFE_MOD = 0x00;
}

void putchar(char c)
{
    CH554UART0SendByte(c);
}
