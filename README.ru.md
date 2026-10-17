# steamvr-arc-dx12-bridge

Переходник, с которым VR-игры на DirectX 11 через OpenVR запускаются в SteamVR на видеокартах
**Intel Arc**. Он передаёт кадры игры в SteamVR как текстуры DirectX 12.

[English version](README.md)

> **Экспериментальный.** Проверен на Intel Arc B580 (драйвер 32.0.101.9150), Windows 10 22H2,
> SteamVR 2.17.10 с игрой Drums Rock и шлемом PICO 4 через ALVR. Будем рады отчётам о других
> играх и видеокартах.

## Проблема

На Intel Arc домашнее пространство SteamVR (пустая комната) работает, а игра на DirectX 11
зависает, как только начинает рисовать: стоит на заставке, а секунд через 5 компоситор
SteamVR падает. Что видно в `C:\Program Files (x86)\Steam\logs`:

- `vrclient_<игра>.txt`: `Failed to open frame info buffer (D3D11)!`
- `vrcompositor.txt`:
  `Failed Watchdog timeout in thread Render in CGraphicsDevice::DoSwapchainPresent after 5.0 seconds. Aborting.`
- Просмотр событий Windows → Приложение, событие 1000: `vrcompositor.exe`, модуль `ucrtbase.dll`,
  код `0xc0000409`.

Шлем и программа стриминга ни при чём: то же самое происходит со встроенным виртуальным шлемом
SteamVR (null-драйвер), когда настоящий шлем вообще не подключён. Игр на DirectX 12 это не
касается.

## Сначала попробуйте DirectX 12

Если игра умеет работать на DirectX 12, запустите её так, и ничего отсюда не понадобится.
Для игр на Unreal Engine добавьте `-dx12` в параметры запуска Steam (Свойства → Общие →
Параметры запуска). Так работает, например, Beyond Sandbox.

Не у всех игр это получится. Drums Rock (Unity) на DirectX 12 запускается, но её OpenVR-плагин
пишет `Unsupported graphics api`, и VR не включается. Для таких игр и нужен переходник.

## Как это работает

Переходник заменяет `openvr_api.dll` игры. Тот же приём использует
[openvr_fsr](https://github.com/fholger/openvr_fsr). Оригинальная DLL переименовывается в
`openvr_api.orig.dll`, и все вызовы передаются ей без изменений, кроме
`IVRCompositor::Submit`:

1. На первом кадре DX11 переходник создаёт устройство D3D12 на той же видеокарте и несколько
   текстур, общих для D3D11 и D3D12, с общими fence для синхронизации.
2. Текстура каждого глаза копируется на видеокарте в свободную общую текстуру: одно
   копирование на глаз за кадр.
3. Кадр отправляется в SteamVR как `TextureType_DirectX12`, по пути, который на Arc работает.

Синхронизация идёт на видеокарте через fence. Процессор ждёт, только если все текстуры заняты.

## Состояние

| Игра | Движок / VR-плагин | Результат |
|---|---|---|
| Drums Rock | Unity 2022.3, OpenVR XR plugin, `IVRCompositor_027` | работает: меню и игровой процесс, 19 000 кадров, 0 ошибок |

Ограничения версии 0.1:

- Конвертируются только игры, которые запрашивают `IVRCompositor_027`. С другими версиями в
  логе будет `WARNING: IVRCompositor_0xx requested ... not hooked`, и игра поведёт себя так же,
  как без переходника.
- C-интерфейс через таблицу функций (`FnTable:IVRCompositor_027`) пока не конвертируется.
- Только 64-битные игры.
- Если игра меняет разрешение рендера на ходу, конвертация отключается до перезапуска игры.
- Текстуры глубины (`Submit_TextureWithDepth`) дальше не передаются.
- Игры на OpenXR (например, Beat Saber) не покрыты: они не используют `openvr_api.dll`.
- Некоторые игры не запускаются с подменённой `openvr_api.dll`. Не используйте переходник в
  играх с античитом.
- «Проверка целостности файлов» в Steam или обновление игры возвращают оригинальную DLL.
  В этом случае запустите установку ещё раз.

## Установка

1. Скачайте репозиторий (Код → Скачать ZIP) и распакуйте.
2. Закройте игру. В PowerShell из распакованной папки выполните:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\install.ps1 "C:\Program Files (x86)\Steam\steamapps\common\<Игра>"
   ```

   Скрипт найдёт `openvr_api.dll` в папке игры, переименует её в `openvr_api.orig.dll` и
   положит рядом `bin\openvr_api.dll` и `openvr_proxy.ini`. Если появится ошибка доступа,
   запустите PowerShell от имени администратора.

Вручную: переименуйте `openvr_api.dll` игры в `openvr_api.orig.dll` и скопируйте в ту же папку
`bin\openvr_api.dll` и `openvr_proxy.ini`.

## Удаление

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1 "C:\Program Files (x86)\Steam\steamapps\common\<Игра>"
```

Ещё можно сделать так: Steam → Свойства игры → Установленные файлы → Проверить целостность
файлов игры.

## Настройки и лог

Файл `openvr_proxy.ini` лежит рядом с DLL:

```ini
[proxy]
convert=1   ; 0 = ничего не менять, удобно для сравнения или чтобы выключить без удаления
mirror=0    ; 1 = разрешить GetMirrorTextureD3D11 (зеркало на мониторе в некоторых играх), не проверено
```

Лог `openvr_proxy.log` пишется рядом с DLL. Если всё в порядке, в нём есть строки
`bridge: sharing via ...` и раз в несколько секунд `stats: submits=... errors=0`.

Если что-то не работает, приложите к сообщению этот лог и файлы `vrcompositor.txt` и
`vrclient_<игра>.txt` из `Steam\logs`.

## Сборка

DLL собирается кросс-компиляцией через [zig](https://ziglang.org/download/) 0.13 на Linux или
Windows: `ZIG=/путь/к/zig ./build.sh`. Нужен компилятор, совместимый с clang. MSVC код в
текущем виде не соберёт.

Технические подробности для разработчиков драйверов и SteamVR — в английском
[README](README.md#technical-notes).

## Лицензия

MIT, см. [LICENSE](LICENSE).
