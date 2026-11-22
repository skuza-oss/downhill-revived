//! What a failed vkCreate*Pipelines becomes (findings 176, 278): every result but
//! VK_ERROR_DEVICE_LOST is that pipeline's own error (wgpu-core: an internal error, the device
//! stays), named in its message. Upstream turns any result but out-of-memory into
//! `DeviceError::Unexpected`, which wgpu-core answers by losing the device: PowerVR B-Series
//! drivers return VK_ERROR_UNKNOWN for some pipelines (issue #119).
#![cfg(all(feature = "native", any(windows, target_os = "linux", target_os = "android")))]

use ash::vk;
use wgpu_hal::vulkan::map_pipeline_creation_err;
use wgpu_hal::{DeviceError, PipelineError};

fn linkage(err: vk::Result) -> String {
    match map_pipeline_creation_err(err, wgpu_types::ShaderStages::VERTEX_FRAGMENT) {
        PipelineError::Linkage(stages, message) => {
            assert_eq!(stages, wgpu_types::ShaderStages::VERTEX_FRAGMENT);
            message
        }
        other => panic!("{err:?}: {other:?}, not the pipeline's own error"),
    }
}

#[test]
fn a_refused_pipeline_is_its_own_error() {
    for (err, name) in [(vk::Result::ERROR_UNKNOWN, "ERROR_UNKNOWN"),
                        (vk::Result::ERROR_OUT_OF_HOST_MEMORY, "ERROR_OUT_OF_HOST_MEMORY"),
                        (vk::Result::ERROR_OUT_OF_DEVICE_MEMORY, "ERROR_OUT_OF_DEVICE_MEMORY"),
                        (vk::Result::ERROR_INITIALIZATION_FAILED, "ERROR_INITIALIZATION_FAILED"),
                        (vk::Result::ERROR_INVALID_SHADER_NV, "ERROR_INVALID_SHADER_NV")] {
        let message = linkage(err);
        assert!(message.contains("the driver did not create the pipeline") && message.contains(name), "{message}");
    }
}

#[test]
fn a_lost_device_stays_lost() {
    assert!(matches!(map_pipeline_creation_err(vk::Result::ERROR_DEVICE_LOST, wgpu_types::ShaderStages::COMPUTE),
                     PipelineError::Device(DeviceError::Lost)));
}
