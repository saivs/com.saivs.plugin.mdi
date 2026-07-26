using UnityEngine;
using Saivs.Graphics.Core.MDI;

namespace Saivs.Graphics.Test
{
    [ExecuteInEditMode]
    public class FPSCounter : MonoBehaviour
    {
        [SerializeField] private MDITestController _controller;

        private float _fps;
        private float _fpsTimer;
        private int _fpsFrames;
        private static GUIStyle _guiStyle;

        private void OnEnable()
        {
            Application.targetFrameRate = -1;
            QualitySettings.vSyncCount = 0;
        }

        private void Update()
        {
            _fpsFrames++;
            _fpsTimer += Time.unscaledDeltaTime;
            if (_fpsTimer >= 0.5f)
            {
                _fps = _fpsFrames / _fpsTimer;
                _fpsFrames = 0;
                _fpsTimer = 0f;
            }
        }

        private void OnGUI()
        {
            if (_guiStyle == null)
            {
                _guiStyle = new GUIStyle(GUI.skin.label)
                {
                    fontStyle = FontStyle.Bold,
                    fontSize = 16
                };
            }

            var drawMode = _controller != null ? _controller.CurrentDrawMode : MDITestController.DrawMode.MultiDrawIndexedIndirect;
            var drawCount = _controller != null ? _controller.BufferManager.DrawCount : 0;

            bool isPluginMode = drawMode == MDITestController.DrawMode.MultiDrawIndexedIndirect
                             || drawMode == MDITestController.DrawMode.MultiDrawIndexedIndirectGpuCount
                             || drawMode == MDITestController.DrawMode.MultiDrawMeshIndirect;

            _guiStyle.normal.textColor = isPluginMode
                ? new Color(0f, 0.9f, 0.1f)
                : new Color(0.9f, 0f, 0.1f);

            GUILayout.BeginArea(new Rect(10, 10, 400, 200));
            GUILayout.Label($"FPS: {_fps:F1}  ({1000f / Mathf.Max(_fps, 0.001f):F2} ms)", _guiStyle);
            GUILayout.Label($"MDI Plugin Supported: {MultiDrawIndirect.IsSupported}", _guiStyle);
            GUILayout.Label($"Draw Mode: {drawMode}", _guiStyle);
            GUILayout.Label($"(Space to switch)", _guiStyle);
            GUILayout.Label($"Draw Commands: {drawCount}", _guiStyle);
            GUILayout.Label($"Graphics API: {SystemInfo.graphicsDeviceType}", _guiStyle);
            GUILayout.EndArea();
        }
    }
}
