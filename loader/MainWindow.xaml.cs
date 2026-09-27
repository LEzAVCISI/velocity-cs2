using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using Microsoft.Win32;

namespace VelocityLoader
{
    public partial class MainWindow : Window
    {
        // Win32 APIs for manual injection & process management
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        private static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out UIntPtr lpNumberOfBytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, IntPtr lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr hObject);

        private const uint PROCESS_ALL_ACCESS = 0x1F0FFF;
        private const uint MEM_COMMIT = 0x1000;
        private const uint MEM_RESERVE = 0x2000;
        private const uint PAGE_READWRITE = 0x04;

        private bool _isClosing = false;

        public MainWindow()
        {
            InitializeComponent();
        }

        private async void Window_Loaded(object sender, RoutedEventArgs e)
        {
            // Opening fade-in (note: RenderTransform is not allowed on a
            // top-level Window, so the entrance stays opacity-only).
            var fadeIn = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(400));
            this.BeginAnimation(OpacityProperty, fadeIn);

            // Display welcome, PCNAME
            string pcName = Environment.MachineName.ToLower();
            WelcomeText.Text = $"welcome, {pcName}";

            // No auth — straight to dashboard after the welcome beat.
            UserWelcomeText.Text = $"User: {pcName}";
            UserStatusText.Text = "Status: LOCAL";
            UserExpiryText.Text = "Remaining: unlimited";

            await Task.Delay(1300);

            var fadeOut = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(300));
            fadeOut.Completed += (s, ev) =>
            {
                WelcomeOverlay.Visibility = Visibility.Collapsed;
                DashboardPanel.Visibility = Visibility.Visible;
                var dashFadeIn = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(300));
                DashboardPanel.BeginAnimation(OpacityProperty, dashFadeIn);
                PlayDashboardEntrance();
            };
            WelcomeOverlay.BeginAnimation(OpacityProperty, fadeOut);
        }

        private void Window_MouseDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton == MouseButton.Left)
            {
                this.DragMove();
            }
        }

        private async void CloseButton_Click(object sender, RoutedEventArgs e)
        {
            await TriggerSayonaraAndExit();
        }

        private async Task TriggerSayonaraAndExit()
        {
            if (_isClosing) return;
            _isClosing = true;

            string pcName = Environment.MachineName.ToLower();
            SayonaraSubText.Text = $"see you later, {pcName}";

            SayonaraOverlay.Visibility = Visibility.Visible;
            SayonaraOverlay.Opacity = 0;

            var fadeIn = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(300));
            SayonaraOverlay.BeginAnimation(OpacityProperty, fadeIn);

            await Task.Delay(1300);

            var windowFadeOut = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(300));
            windowFadeOut.Completed += (s, ev) =>
            {
                Application.Current.Shutdown();
            };
            this.BeginAnimation(OpacityProperty, windowFadeOut);
        }

        private void AnimateIn(FrameworkElement el, double delayMs)
        {
            el.Opacity = 0;
            var sb = new Storyboard();

            var fade = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(320))
            {
                BeginTime = TimeSpan.FromMilliseconds(delayMs)
            };
            Storyboard.SetTarget(fade, el);
            Storyboard.SetTargetProperty(fade, new PropertyPath(UIElement.OpacityProperty));
            sb.Children.Add(fade);

            if (el.RenderTransform is TranslateTransform tt)
            {
                var slide = new DoubleAnimation(10, 0, TimeSpan.FromMilliseconds(360))
                {
                    BeginTime = TimeSpan.FromMilliseconds(delayMs),
                    EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
                };
                Storyboard.SetTarget(slide, tt);
                Storyboard.SetTargetProperty(slide, new PropertyPath(TranslateTransform.YProperty));
                sb.Children.Add(slide);
            }

            sb.Begin();
        }

        private void PlayDashboardEntrance()
        {
            AnimateIn(UserInfoCard, 0);
            AnimateIn(LoadBtn, 90);
            AnimateIn(LoaderStatusText, 180);
            if (LoadProgressBar.Visibility == Visibility.Visible)
                AnimateIn(LoadProgressBar, 240);
        }

        private void SetLoadProgress(bool show)
        {
            Dispatcher.Invoke(() =>
            {
                if (show)
                {
                    LoadProgressBar.Visibility = Visibility.Visible;
                    LoadProgressBar.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(250)));
                }
                else
                {
                    var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(200));
                    fade.Completed += (s, ev) => LoadProgressBar.Visibility = Visibility.Collapsed;
                    LoadProgressBar.BeginAnimation(OpacityProperty, fade);
                }
            });
        }

        private void SetLoaderStatus(string text, Brush? color = null)
        {
            Dispatcher.Invoke(() =>
            {
                if (color != null)
                    LoaderStatusText.Foreground = color;
                if (LoaderStatusText.Text == text)
                    return;
                var fadeOut = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(120));
                fadeOut.Completed += (s, ev) =>
                {
                    LoaderStatusText.Text = text;
                    LoaderStatusText.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(220)));
                };
                LoaderStatusText.BeginAnimation(OpacityProperty, fadeOut);
            });
        }


        private async void LoadBtn_Click(object sender, RoutedEventArgs e)
        {
            LoadBtn.IsEnabled = false;
            LoadProgressBar.IsIndeterminate = true;
            SetLoadProgress(true);
            SetLoaderStatus("Waiting for Counter-Strike 2 (cs2.exe)...", (Brush)FindResource("AccentGold"));

            await Task.Run(async () =>
            {
                Process? targetProcess = null;

                // Poll for CS2 process
                while (targetProcess == null)
                {
                    var processes = Process.GetProcessesByName("cs2");
                    if (processes.Length > 0)
                    {
                        targetProcess = processes[0];
                        break;
                    }
                    await Task.Delay(1000);
                }

                SetLoaderStatus($"Found CS2 (PID {targetProcess.Id}). Preparing injection...");

                await Task.Delay(1000);

                // Locate cs2.dll
                string currentDir = AppDomain.CurrentDomain.BaseDirectory;
                string dllPath = Path.Combine(currentDir, "cs2.dll");
                if (!File.Exists(dllPath))
                {
                    // Fallback to parent bin folder if running from build directory
                    string parentBinPath = Path.GetFullPath(Path.Combine(currentDir, @"..\..\..\..\bin\cs2.dll"));
                    if (File.Exists(parentBinPath))
                    {
                        dllPath = parentBinPath;
                    }
                }

                if (!File.Exists(dllPath))
                {
                    SetLoadProgress(false);
                    SetLoaderStatus("Error: cs2.dll not found.", Brushes.Red);
                    Dispatcher.Invoke(() => LoadBtn.IsEnabled = true);
                    return;
                }

                bool success = InjectDll(targetProcess.Id, dllPath);

                SetLoadProgress(false);
                if (success)
                {
                    SetLoaderStatus("Cheat injected successfully! Enjoy.", Brushes.LightGreen);
                }
                else
                {
                    SetLoaderStatus("Injection failed. Make sure to run as Administrator.", Brushes.Red);
                    Dispatcher.Invoke(() => LoadBtn.IsEnabled = true);
                }

                if (success)
                {
                    await Task.Delay(1500);
                    Dispatcher.Invoke(async () => await TriggerSayonaraAndExit());
                }
            });
        }

        private static bool InjectDll(int processId, string dllPath)
        {
            IntPtr hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, processId);
            if (hProcess == IntPtr.Zero)
            {
                return false;
            }

            try
            {
                byte[] pathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
                IntPtr allocMemAddress = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)pathBytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (allocMemAddress == IntPtr.Zero)
                {
                    return false;
                }

                if (!WriteProcessMemory(hProcess, allocMemAddress, pathBytes, (uint)pathBytes.Length, out _))
                {
                    return false;
                }

                IntPtr loadLibraryAddr = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
                if (loadLibraryAddr == IntPtr.Zero)
                {
                    return false;
                }

                IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, loadLibraryAddr, allocMemAddress, 0, IntPtr.Zero);
                if (hThread == IntPtr.Zero)
                {
                    return false;
                }

                CloseHandle(hThread);
                return true;
            }
            finally
            {
                CloseHandle(hProcess);
            }
        }
    }
}
