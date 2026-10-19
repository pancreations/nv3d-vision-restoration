using System;
using System.Collections;
using System.Collections.Generic;
using MelonLoader;
using UnityEngine;
using UnityEngine.Rendering;

[assembly: MelonInfo(typeof(ScheduleStereo.StereoMod), "Schedule I Stereo SBS", "0.1.0", "Vision Restoration")]
[assembly: MelonGame("TVGS", "Schedule I")]

namespace ScheduleStereo
{
    public sealed class StereoMod : MelonMod
    {
        MelonPreferences_Category prefs;
        MelonPreferences_Entry<bool> enabled, swap;
        MelonPreferences_Entry<float> ipd, convergence, scale;
        RenderTexture left, right;
        bool pairReady, faulted;
        int width, height;
        public override void OnInitializeMelon()
        {
            prefs = MelonPreferences.CreateCategory("ScheduleIStereoSBS");
            enabled = prefs.CreateEntry("Enabled", true);
            swap = prefs.CreateEntry("SwapEyes", false);
            ipd = prefs.CreateEntry("EyeSeparationMeters", 0.064f);
            convergence = prefs.CreateEntry("ConvergenceMeters", 3f);
            scale = prefs.CreateEntry("RenderScale", 0.75f);
            Application.runInBackground = true;
            MelonCoroutines.Start(RenderLoop());
            LoggerInstance.Msg("Half SBS: F6 toggle, F7 swap; Ctrl+arrows adjust stereo; F8 borderless.");
        }
        public override void OnUpdate()
        {
            if (Input.GetKeyDown(KeyCode.F6)) { enabled.Value = !enabled.Value; faulted = false; Save(); }
            if (Input.GetKeyDown(KeyCode.F7)) { swap.Value = !swap.Value; Save(); }
            if (Input.GetKeyDown(KeyCode.F8)) Borderless();
            if (Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl))
            {
                bool changed = false;
                if (Input.GetKeyDown(KeyCode.LeftArrow)) { ipd.Value -= .005f; changed = true; }
                if (Input.GetKeyDown(KeyCode.RightArrow)) { ipd.Value += .005f; changed = true; }
                if (Input.GetKeyDown(KeyCode.UpArrow)) { convergence.Value += .5f; changed = true; }
                if (Input.GetKeyDown(KeyCode.DownArrow)) { convergence.Value -= .5f; changed = true; }
                if (changed) { ipd.Value = Mathf.Clamp(ipd.Value, 0, .2f); convergence.Value = Mathf.Clamp(convergence.Value, .25f, 100); Save(); }
            }
        }
        void Save() { prefs.SaveToFile(false); LoggerInstance.Msg($"SBS={enabled.Value}, swap={swap.Value}, separation={ipd.Value:F3}m, convergence={convergence.Value:F1}m"); }
        void Borderless()
        {
            var res = Screen.currentResolution;
            Screen.SetResolution(res.width, res.height, FullScreenMode.FullScreenWindow);
        }
        IEnumerator RenderLoop()
        {
            yield return new WaitForSeconds(3);
            Borderless();
            while (true)
            {
                yield return new WaitForEndOfFrame();
                if (!enabled.Value || faulted) { pairReady = false; continue; }
                try { RenderPair(); }
                catch (Exception ex)
                {
                    pairReady = false; faulted = true;
                    LoggerInstance.Error("Stereo disabled after rendering failure; normal game view restored. F6 retries. " + ex);
                }
            }
        }
        void Release()
        {
            pairReady = false;
            if (left != null) { left.Release(); UnityEngine.Object.Destroy(left); left = null; }
            if (right != null) { right.Release(); UnityEngine.Object.Destroy(right); right = null; }
        }
        RenderTexture MakeTexture()
        {
            var rt = new RenderTexture(width, height, 24, RenderTextureFormat.ARGB32);
            rt.antiAliasing = 1; rt.useMipMap = false; rt.Create(); return rt;
        }
        void RenderPair()
        {
            Camera camera = Camera.main;
            if (camera == null)
                foreach (var candidate in Camera.allCameras)
                    if (candidate.isActiveAndEnabled && !candidate.orthographic && candidate.targetTexture == null && candidate.cameraType == CameraType.Game)
                        if (camera == null || candidate.depth < camera.depth) camera = candidate;
            if (camera == null || !camera.isActiveAndEnabled || camera.orthographic || camera.targetTexture != null) { pairReady = false; return; }
            int w = Mathf.Max(64, Mathf.RoundToInt(Screen.width * Mathf.Clamp(scale.Value, .25f, 1f)));
            int h = Mathf.Max(64, Mathf.RoundToInt(Screen.height * Mathf.Clamp(scale.Value, .25f, 1f)));
            if (left == null || w != width || h != height)
            {
                Release(); width = w; height = h; left = MakeTexture(); right = MakeTexture();
                LoggerInstance.Msg($"Stereo eye targets {width}x{height}; output Half SBS {Screen.width}x{Screen.height}; camera {camera.name}");
            }
            // Use the game's camera and full URP stack, preserving effects and overlays.
            var view = camera.worldToCameraMatrix;
            var projection = camera.projectionMatrix;
            var active = RenderTexture.active;
            var overlays = new List<(Canvas canvas, Camera camera, float distance)>();
            // The IL2CPP build strips the managed constructor; allocate the request natively.
            var request = new RenderPipeline.StandardRequest(Il2CppInterop.Runtime.IL2CPP.il2cpp_object_new(
                Il2CppInterop.Runtime.Il2CppClassPointerStore<RenderPipeline.StandardRequest>.NativeClassPtr));
            request.face = CubemapFace.Unknown;
            if (!RenderPipeline.SupportsRenderRequest(camera, request)) throw new NotSupportedException("The game's pipeline does not support StandardRequest.");
            try
            {
                // Overlay UI normally bypasses camera targets. Include it in both eye requests,
                // then restore before the next frame's input processing.
                foreach (var canvas in UnityEngine.Object.FindObjectsOfType<Canvas>())
                {
                    if (!canvas.isRootCanvas || !canvas.isActiveAndEnabled || canvas.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                    overlays.Add((canvas, canvas.worldCamera, canvas.planeDistance));
                    canvas.worldCamera = camera;
                    canvas.planeDistance = Mathf.Clamp(convergence.Value, camera.nearClipPlane + .1f, camera.farClipPlane - .1f);
                    canvas.renderMode = RenderMode.ScreenSpaceCamera;
                }
                Canvas.ForceUpdateCanvases();
                RenderEye(camera, view, projection, -.5f * Mathf.Clamp(ipd.Value, 0, .2f), left, request);
                RenderEye(camera, view, projection, .5f * Mathf.Clamp(ipd.Value, 0, .2f), right, request);
                pairReady = true;
            }
            finally
            {
                foreach (var item in overlays)
                {
                    if (item.canvas == null) continue;
                    item.canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                    item.canvas.worldCamera = item.camera;
                    item.canvas.planeDistance = item.distance;
                }
                Canvas.ForceUpdateCanvases();
                camera.ResetWorldToCameraMatrix(); camera.ResetProjectionMatrix();
                // Preserve nonstandard matrices supplied by a game effect.
                if (!Approximately(camera.worldToCameraMatrix, view)) camera.worldToCameraMatrix = view;
                if (!Approximately(camera.projectionMatrix, projection)) camera.projectionMatrix = projection;
                RenderTexture.active = active;
            }
            if (!pairReady) return;
            // Composite after Unity has drawn its cameras and UI. No desktop capture or app hook.
            RenderTexture.active = null;
            GL.PushMatrix();
            try
            {
                GL.LoadPixelMatrix(0, Screen.width, Screen.height, 0);
                Graphics.DrawTexture(new Rect(0, 0, Screen.width / 2f, Screen.height), swap.Value ? right : left);
                Graphics.DrawTexture(new Rect(Screen.width / 2f, 0, Screen.width / 2f, Screen.height), swap.Value ? left : right);
            }
            finally { GL.PopMatrix(); RenderTexture.active = active; }
        }
        static bool Approximately(Matrix4x4 a, Matrix4x4 b)
        {
            for (int row = 0; row < 4; row++) for (int col = 0; col < 4; col++)
                if (Mathf.Abs(a.get_Item(row, col) - b.get_Item(row, col)) > .00001f) return false;
            return true;
        }
        void RenderEye(Camera camera, Matrix4x4 view, Matrix4x4 projection, float eye, RenderTexture destination, RenderPipeline.StandardRequest request)
        {
            // Parallel cameras with asymmetric frusta: no toe-in/vertical disparity.
            var v = view; v.m03 -= eye;
            var p = projection; p.m02 -= eye * p.m00 / Mathf.Clamp(convergence.Value, .25f, 100);
            camera.worldToCameraMatrix = v; camera.projectionMatrix = p;
            request.destination = destination;
            // MelonLoader's generated generic SubmitRenderRequest wrapper fails verification
            // on Unity 2022.3.62's ObjectIdRequest. Submit the already-validated SRP request
            // through Unity's native binding instead.
            camera.SubmitRenderRequestsInternal(request);
        }
        public override void OnDeinitializeMelon() { Release(); }
    }
}
