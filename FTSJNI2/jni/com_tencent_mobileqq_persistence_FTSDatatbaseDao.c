#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/com_tencent_mobileqq_persistence_FTSDatatbaseDao.h"
#include "include/utils.h"
#include "include/sqlite3.h"
#include "include/fts3_tokenizer.h"
#include "include/base64.h"

// dbName和Java层保持一致
#define DB_FILE "/data/data/com.tencent.mobileqq/databases/IndexQQMsg.db"

void qqcompress(sqlite3_context* context, int argc, sqlite3_value** argv);
void qquncompress(sqlite3_context* context, int argc, sqlite3_value** argv);

static sqlite3* db = NULL;
static sqlite3_stmt* stmt = NULL;

extern char* getSegmentedMsg(char* msg);

jint Java_com_tencent_mobileqq_persistence_FTSDatatbaseDao_initFTS(JNIEnv* env, jobject thiz)
{
    int errCode = sqlite3_open_v2(DB_FILE, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (SQLITE_OK != errCode)
    {
        logError("Can't open database IndexQQMsg.db, ", sqlite3_errmsg(db));

        sqlite3_close(db);
        return errCode;
    }

    errCode = sqlite3_create_function(db, "qqcompress", 1, SQLITE_UTF8, NULL, qqcompress, NULL, NULL);
    if (SQLITE_OK != errCode)
    {
        logError("Can't create function, ", sqlite3_errmsg(db));

        sqlite3_close(db);
        return errCode;
    }

    errCode = sqlite3_create_function(db, "qquncompress", 1, SQLITE_UTF8, NULL, qquncompress, NULL, NULL);
    if (SQLITE_OK != errCode)
    {
        logError("Can't create function, ", sqlite3_errmsg(db));

        sqlite3_close(db);
        return errCode;
    }

    char* sql = "CREATE VIRTUAL TABLE IF NOT EXISTS IndexMsg USING fts4(uin, istroop, time, shmsgseq, msg, msgindex, compress=qqcompress, uncompress=qquncompress);";
    errCode = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (SQLITE_OK != errCode)
    {
        logError("Can't create virtual table IndexMsg, ", sqlite3_errmsg(db));

        sqlite3_close(db);
        return errCode;
    }

    logInfo("FTS init...", NULL);
    return 0;
}


// 事务写，参数绑定留待后续
jint Java_com_tencent_mobileqq_persistence_FTSDatatbaseDao_insertFTS(JNIEnv* env, jobject thiz, jlong juin, jint jistroop, jlong jtime, jlong jshmsgseq, jstring jmsg)
{
    long long uin = (long long) juin;
    int istroop = (int) jistroop;
    long long msgtime = (long long) jtime;
    long long shmsgseq = (long long) jshmsgseq;
    const char* msg = (*env)->GetStringUTFChars(env, jmsg, NULL);

    // 分词，再组装
    char* segments = getSegmentedMsg(msg);
    if (NULL == segments || strlen(segments) == 0)
    {
        logWarn("FTS insert: msg is null...", NULL);
        return SQLITE_OK;
    }
    logInfo("FTS insert: segments = ", segments);

    // 创建sqlite3_stmt
    if (NULL == stmt)
    {
        char* zSql = "INSERT INTO IndexMsg(uin, istroop, time, shmsgseq, msg, msgindex) VALUES(?, ?, ?, ?, ?, ?);";
        int rc = sqlite3_prepare_v2(db, zSql, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            logError(sqlite3_errmsg(db), NULL);

            sqlite3_close(db);
            return rc;
        }
    }

    sqlite3_bind_int64(stmt, 1, uin);

    sqlite3_bind_int(stmt, 2, istroop);

    sqlite3_bind_int64(stmt, 3, msgtime);

    sqlite3_bind_int64(stmt, 4, shmsgseq);

    sqlite3_bind_text(stmt, 5, msg, -1, SQLITE_STATIC);

    sqlite3_bind_text(stmt, 6, segments, -1, SQLITE_STATIC);

    sqlite3_step(stmt);

    sqlite3_reset(stmt);

    free(segments);
    (*env)->ReleaseStringUTFChars(env, jmsg, msg);

    logInfo("FTS insert...", NULL);

    return 0;
}

jobject Java_com_tencent_mobileqq_persistence_FTSDatatbaseDao_queryFTSGroups(JNIEnv* env, jobject thiz, jstring jsql, jstring jclasspath)
{
    // 获取ArrayList class类
    jclass list_clazz = (*env)->FindClass(env, "java/util/ArrayList");

    // 获取ArrayList类构造函数ID
    jmethodID list_init = (*env)->GetMethodID(env, list_clazz , "<init>", "()V");

    // 获取ArrayList类add函数ID
    jmethodID list_add  = (*env)->GetMethodID(env, list_clazz, "add", "(Ljava/lang/Object;)Z");

    // 构造ArrayList对象
    jobject list_obj = (*env)->NewObject(env, list_clazz , list_init);

    // 获取FTSMsgGroupItem class类
    const char* classpath = (*env)->GetStringUTFChars(env, jclasspath, NULL);
    jclass group_clazz = (*env)->FindClass(env, classpath);
    (*env)->ReleaseStringUTFChars(env, jclasspath, classpath);

    // 获取FTSMsgGroupItem类构造函数ID
    jmethodID group_init = (*env)->GetMethodID(env, group_clazz , "<init>", "(JII)V");

    // 搜索
    char** result;
    int nrows;
    int ncols;
    const char* sql = (*env)->GetStringUTFChars(env, jsql, NULL);
    int rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, NULL);
    (*env)->ReleaseStringUTFChars(env, jsql, sql);
    if (rc != SQLITE_OK)
    {
        logError(sqlite3_errmsg(db), NULL);

        sqlite3_close(db);
        return list_obj;
    }

    // 搜索结果为空
    if (nrows == 0)
    {
        logInfo("FTS queryFTSGroups: nrows = 0", NULL);

        sqlite3_free_table(result);
        return list_obj;
    }

    // SQL查询语句，SELECT三个字段：uin、istroop、counts
    if (ncols != 3)
    {
        logWarn("FTS queryFTSGroups: ncols != 3", NULL);

        sqlite3_free_table(result);
        return list_obj;
    }

    int i;
    for (i = 0; i < nrows; ++i)
    {
        // 注意：java long和c/c++ long，不一样，小心掉坑里！！
        long long uin = atoll(result[(i + 1) * ncols + 0]);

        int istroop = atoi(result[(i + 1) * ncols + 1]);

        int counts = atoi(result[(i + 1) * ncols + 2]);

        // 构造FTSMsgGroupItem对象
        jobject group_obj = (*env)->NewObject(env, group_clazz, group_init, uin, istroop, counts);

        (*env)->CallBooleanMethod(env, list_obj, list_add, group_obj);
    }

    sqlite3_free_table(result);
    return list_obj;
}

jobject Java_com_tencent_mobileqq_persistence_FTSDatatbaseDao_queryFTSMsgs(JNIEnv* env, jobject thiz, jstring jsql, jstring jclasspath)
{
    // 获取ArrayList class类
    jclass list_clazz = (*env)->FindClass(env, "java/util/ArrayList");

    // 获取ArrayList类构造函数ID
    jmethodID list_init = (*env)->GetMethodID(env, list_clazz , "<init>", "()V");

    // 获取ArrayList类add函数ID
    jmethodID list_add  = (*env)->GetMethodID(env, list_clazz, "add", "(Ljava/lang/Object;)Z");

    // 构造ArrayList对象
    jobject list_obj = (*env)->NewObject(env, list_clazz , list_init);

    // 获取FTSMsgItem class类
    const char* classpath = (*env)->GetStringUTFChars(env, jclasspath, NULL);
    jclass msg_clazz = (*env)->FindClass(env, classpath);
    (*env)->ReleaseStringUTFChars(env, jclasspath, classpath);

    // 获取FTSMsgItem类构造函数ID
    jmethodID msg_init = (*env)->GetMethodID(env, msg_clazz , "<init>", "(JIJJLjava/lang/String;)V");

    // 搜索
    char** result;
    int nrows;
    int ncols;
    const char* sql = (*env)->GetStringUTFChars(env, jsql, NULL);
    int rc = sqlite3_get_table(db, sql, &result, &nrows, &ncols, NULL);
    (*env)->ReleaseStringUTFChars(env, jsql, sql);
    if (rc != SQLITE_OK)
    {
        logError(sqlite3_errmsg(db), NULL);

        sqlite3_close(db);
        return list_obj;
    }

    // 搜索结果为空
    if (nrows == 0)
    {
        logInfo("FTS queryFTSMsgs: nrows = 0", NULL);

        sqlite3_free_table(result);
        return list_obj;
    }

    // SQL查询语句，SELECT五个字段：uin、istroop、time、shmsgseq，msg
    if (ncols != 5)
    {
        logWarn("FTS queryFTSMsgs: ncols != 5", NULL);

        sqlite3_free_table(result);
        return list_obj;
    }

    int i;
    for (i = 0; i < nrows; ++i)
    {
        // 注意：java long和c/c++ long，不一样，小心掉坑里！！
        long long uin = atoll(result[(i + 1) * ncols + 0]);

        int istroop = atoi(result[(i + 1) * ncols + 1]);

        long long time = atoll(result[(i + 1) * ncols + 2]);

        long long shmsgseq = atoll(result[(i + 1) * ncols + 3]);

        jstring msg = (*env)->NewStringUTF(env, result[(i + 1) * ncols + 4]);

        // 构造FTSMsgItem对象
        jobject msg_obj = (*env)->NewObject(env, msg_clazz, msg_init, uin, istroop, time, shmsgseq, msg);

        (*env)->CallBooleanMethod(env, list_obj, list_add, msg_obj);
    }

    return list_obj;
}

jint Java_com_tencent_mobileqq_persistence_FTSDatatbaseDao_closeFTS(JNIEnv* env, jobject thiz)
{
    if (db != NULL)
    {
        sqlite3_close(db);
        db = NULL;
    }

    if (stmt != NULL)
    {
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    logInfo("FTS close...", NULL);
    return 0;
}

jstring Java_com_tencent_mobileqq_persistence_FTSDatatbaseDao_wordSegment(JNIEnv* env, jclass clazz, jstring jsearch)
{
    const char* msg = (*env)->GetStringUTFChars(env, jsearch, NULL);

    // 分词，再组装
    char* segments = getSegmentedMsg(msg);
    if (NULL == segments || strlen(segments) == 0)
    {
        logWarn("FTS word segment: msg is null...", NULL);
        return NULL;
    }
    // logInfo("FTS word segment: segments = ", segments);

    jstring jsegments = (*env)->NewStringUTF(env, segments);

    free(segments);
    (*env)->ReleaseStringUTFChars(env, jsearch, msg);

    // logInfo("FTS word segment...", NULL);

    return jsegments;
}

void qqcompress(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    const char* msg = sqlite3_value_text(argv[0]);

    // logInfo("compress before: ", msg);

    char* msg2 = base64_encode(msg);

    // logInfo("compress after: ", msg2);

    sqlite3_result_text(context, msg2, strlen(msg2), sqlite3_free);
}

void qquncompress(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    const char* msg = sqlite3_value_text(argv[0]);

    // logInfo("uncompress before: ", msg);

    char* msg2 = base64_decode(msg);

    // logInfo("uncompress after: ", msg2);

    sqlite3_result_text(context, msg2, strlen(msg2), sqlite3_free);
}
