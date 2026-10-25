// Screen updates off while an export or import drives SolidWorks. Without this every API call redraws the
// graphics and the FeatureManager tree of the document it changes, which costs far more than the call itself.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using SolidWorks.Interop.sldworks;

namespace Ncx.Sw
{
    public sealed class QuietUpdates : IDisposable
    {
        readonly ISldWorks sw;
        readonly bool commandInProgress;
        readonly List<IModelDoc2> docs = new List<IModelDoc2>();

        public QuietUpdates(ISldWorks sw)
        {
            this.sw = sw;
            commandInProgress = sw.CommandInProgress;
            sw.CommandInProgress = true;
        }

        /// <summary>Stop redrawing this document's graphics and feature tree (until Release or Dispose).</summary>
        public void Add(IModelDoc2 d)
        {
            if (d == null || docs.Contains(d)) return;
            try
            {
                ((IFeatureManager)d.FeatureManager).EnableFeatureTree = false;
                var view = d.ActiveView as IModelView;
                if (view != null) view.EnableGraphicsUpdate = false;
                docs.Add(d);
            }
            catch (COMException) { }
        }

        /// <summary>Redraw this document again (before it is saved, so its preview is drawn, or left open for the user).</summary>
        public void Release(IModelDoc2 d)
        {
            if (d == null || !docs.Remove(d)) return;
            Restore(d);
        }

        static void Restore(IModelDoc2 d)
        {
            try
            {
                var view = d.ActiveView as IModelView;
                if (view != null) view.EnableGraphicsUpdate = true;
                var fm = (IFeatureManager)d.FeatureManager;
                fm.EnableFeatureTree = true;
                fm.UpdateFeatureTree();
                d.GraphicsRedraw2();
            }
            catch (COMException) { }   // closed meanwhile
            catch (InvalidComObjectException) { }
        }

        public void Dispose()
        {
            foreach (var d in docs) Restore(d);
            docs.Clear();
            sw.CommandInProgress = commandInProgress;
        }
    }
}
