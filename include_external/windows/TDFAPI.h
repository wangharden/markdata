#ifndef __TDF_API_H__
#define __TDF_API_H__

#include "TDFAPIStruct.h"

#if defined(WIN32) || defined(WIN64) || defined(_WINDOWS)
#ifdef TDF_API_EXPORT
#define TDFAPI __declspec(dllexport) 
#else	
#define TDFAPI __declspec(dllimport)
#endif
#else
#define TDFAPI __attribute((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum TDF_ERR
{
    TDF_ERR_UNKOWN=-200,                // δ֪����

    TDF_ERR_INITIALIZE_FAILURE = -100,  // ��ʼ��socket����ʧ��
    TDF_ERR_NETWORK_ERROR,              // �������ӳ�������
    TDF_ERR_INVALID_PARAMS,             // ���������Ч
    TDF_ERR_VERIFY_FAILURE,             // ��½��֤ʧ�ܣ�ԭ��Ϊ�û�������������󣻳�����½����
    TDF_ERR_NO_AUTHORIZED_MARKET,       // ����������г���û����Ȩ
    TDF_ERR_NO_CODE_TABLE,              // ����������г����춼û�д����
    
    TDF_ERR_SUCCESS = 0,                // �ɹ�
};


//����TDF��������ֵ,�ڵ���TDF_Open֮ǰ����
//����ֵ��TDF_ERR_INVALID_PARAMS��ʾ��Ч��nEnv��TDF_ERR_SUCCESS��ʾ�ɹ�
TDFAPI int TDF_SetEnv(TDF_ENVIRON_SETTING nEnv, unsigned int nValue);

//������־�ļ�·��
TDFAPI int TDF_SetLogPath(const char* path);


//ͬ���������򿪵�TDFServer�����ӣ�����ɹ����򷵻ؾ�������򷵻�NULL����TDF_Open�ڼ䷢��������Ͽ����������Զ�����
//�ڵ����ڼ䣬ϵͳ֪ͨ�������յ�MSG_SYS_CONNECT_RESULT��MSG_SYS_LOGIN_RESULT��MSG_SYS_CODETABLE_RESULT��Ϣ
//�������Ͽ�������յ�MSG_SYS_DISCONNECT_NETWORK��pErr�д�Ŵ������
TDFAPI THANDLE TDF_Open(TDF_OPEN_SETTING* pSettings, TDF_ERR* pErr);
//�������ö����������ȡ�����������������������
TDFAPI THANDLE TDF_OpenExt(TDF_OPEN_SETTING_EXT* pSettings,TDF_ERR* pErr);
//ͨ�����������ӣ��ص���Ϣ�ʹ�������TDF_Openһ��
TDFAPI THANDLE TDF_OpenProxy(TDF_OPEN_SETTING* pOpenSettings, TDF_PROXY_SETTING* pProxySettings, TDF_ERR* pErr);
TDFAPI THANDLE TDF_OpenProxyExt(TDF_OPEN_SETTING_EXT* pOpenSettings, TDF_PROXY_SETTING* pProxySettings, TDF_ERR* pErr);

//��ȡָ���г��Ĵ���������Ѿ��յ�MSG_SYS_CODETABLE_RESULT ��Ϣ֮�󣬿��Ի�ô����
//szMarket��ʽΪ��market-level-source(SHF-1-0)
//��ȡ���Ĵ��������Ҫ����TDF_FreeArr���ͷ��ڴ�
TDFAPI int TDF_GetCodeTable(THANDLE hTdf, const char* szMarket, TDF_CODE** pCode, unsigned int* pItems);

// ����ô�������ȡ��ϸ����Ȩ������Ϣ
// pCodeInfoָ�����û��ṩ��
// ����ɹ���ȡ���򷵻�TDF_ERR_SUCCESS�����򷵻� TDF_ERR_NO_CODE_TABLE �� TDF_ERR_INVALID_PARAMS
// szCode ��ʽΪԭʼcode + . + �г�(��ag.SHF)
TDFAPI int TDF_GetOptionCodeInfo(THANDLE hTdf, const char* szCode, TDF_OPTION_CODE* pCodeInfo, const char* szMarket);

//ͬ���������ر����ӣ���Ҫ�ڻص�����������ã�����Ῠ��
TDFAPI int TDF_Close(THANDLE hTdf);

TDFAPI void TDF_FreeArr(void *pArr);

//��½����; �˺����Ǹ��첽��������TDF_Open�ɹ�֮�����
//���˷����ڷ���ˣ��ú������ú󣬶����б����б仯������˻ᷢ�����ж��Ĵ�������¿���
//szSubScriptions:��Ҫ���ĵĹ�Ʊ(������Ʊ��ʽΪԭʼCode+.+�г�����999999.SH)���ԡ�;���ָ����"600000.SH;ag.SHF;000001.SZ"
TDFAPI int TDF_SetSubscription(THANDLE hTdf, const char* szSubScriptions, SUBSCRIPTION_STYLE nSubStyle);

//�򿪻�ȡ�ο����ݵķ���
TDFAPI THANDLE TDF_Open_RefData(TDF_OPEN_REFDATA_SETTING* pSettings, TDF_ERR* pErr);

//��ȡETF�嵥,szMarketAbbr��ʽΪΪSH
//reqCode��ʽΪԭʼcode(999999)
TDFAPI int TDF_ReqETFList(THANDLE hTdf, int reqID, const char* szMarketAbbr, const char* reqCode, int reqDate, int dataLevel, int marektSource = 0);

//��ȡ����״̬��Ϣ
TDFAPI int TDF_GetConStatInfo(THANDLE hTdf, int conIndex, ConStatInfo* pConsStatInfo);

//�ͷ�ȫ�ֵ��ڴ棺���������˳�ʱ�������Ҫ�����ڴ���䣬�ɵ����ͷ�ȫ���ڴ�. ���ú�API�ⲻ����
TDFAPI void TDF_EXIT();
//��ȡ��ǰAPI�汾��
TDFAPI const char* TDF_Version();

//////////////////////////////////����Ϊ������ȡ����ӿ�//////////////////////////////////////////
//��ȡ��ǰ��������,szMarket��ʽΪmarket-level-source,�����TDF_FreeArr(void*)�ͷ��ڴ�
TDFAPI int TDF_GetLastMarketData(THANDLE hTdf, const char* szMarket, TDF_MARKET_DATA** pMarketData, int* nItems);
//��ȡ��ǰ����ָ��,szMarket��ʽΪmarket-level-source,�����TDF_FreeArr(void*)�ͷ��ڴ�
TDFAPI int TDF_GetLastIndexData(THANDLE hTdf, const char* szMarket, TDF_INDEX_DATA** pIndexData, int* nItems);
//��ȡ��ǰ�����ڻ�(��Ȩ),szMarket��ʽΪmarket-level-source,�����TDF_FreeArr(void*)�ͷ��ڴ�
TDFAPI int TDF_GetLastFutureData(THANDLE hTdf, const char* szMarket, TDF_FUTURE_DATA** pFutureData, int* nItems);
//��ȡ��ǰ���¿���,�ں�ģʽʹ��,szMarket��ʽΪmarket-level-source,�����TDF_FreeArr(void*)�ͷ��ڴ�
TDFAPI int TDF_GetLastSnapShot(THANDLE hTdf, const char* szMarket, void** pSnapShot, unsigned int* nItems);
//����windcode��ȡ��ǰ���¿���,�ں�ģʽʹ��,szMarket��ʽΪmarket-level-source,�����TDF_FreeArr(void*)�ͷ��ڴ�
TDFAPI int TDF_GetSnapShotByWindcode(THANDLE hTdf, const char* szMarket, void** pOneSnapShot, const char* szWindCode);

#ifdef __cplusplus
}
#endif

#endif