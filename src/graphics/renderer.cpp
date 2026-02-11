#include "renderer.hpp"

i8 AE_Renderer::get_valid_physical_device(
    const View<VkPhysicalDevice>&& devices)
{
    VkPhysicalDeviceProperties device_properties;
    VkPhysicalDeviceFeatures   device_features;

    i8 result = -1;

    for (i8 i = 0; i < (i8)devices.NumElements; i++)
    {
        const VkPhysicalDevice& device = devices[i];

        // fill prop and features
        vkGetPhysicalDeviceProperties(device, &device_properties);
        vkGetPhysicalDeviceFeatures(device, &device_features);

        if (device_properties.deviceType ==
                VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            device_properties.apiVersion >= VK_API_VERSION_1_3)
        {
            result = i;

            renderObjects.limits = device_properties.limits;
            break;
        }
    }
    return result;
}
