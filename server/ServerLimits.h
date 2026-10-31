#ifndef LIMITS_H
#define LIMITS_H

// Серверные ограничения против перебора паролей и перегрузки. Ограничения на данные
// пользователя (длина ника, пароля, сообщения) общие с клиентом и лежат в Protocol.h.
namespace Limits {

// Неудачные попытки входа/регистрации с одного адреса
inline constexpr int AuthMaxFailures = 5;
inline constexpr int AuthFailureWindowSec = 600;

// Неаутентифицированное соединение
inline constexpr int AuthTimeoutMs = 30000;
inline constexpr int MaxUnauthFrameSize = 4096;

// Соединения
inline constexpr int MaxConnectionsPerIp = 20;
inline constexpr int MaxConnectionsTotal = 200;

// Сообщения от одного соединения: всплеск и средняя скорость в секунду
inline constexpr int MessageBurst = 60;
inline constexpr double MessagesPerSecond = 20.0;

inline constexpr int MaxClientMsgIdLength = 64;
inline constexpr int MaxAckIds = 1000;

}

#endif
