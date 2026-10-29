import * as THREE from 'three';

/** 静态同材质枝干合为一次绘制，保留原顶点、法线、纹理与轮廓。 */
export function mergeWood(world, material) {
  world.updateMatrixWorld(true);
  const inverse = world.matrixWorld.clone().invert(), parts = [], meshes = [];
  world.traverse(mesh => {
    if (!mesh.isMesh || mesh.isInstancedMesh || mesh.material !== material) return;
    const geometry = mesh.geometry.clone();
    geometry.applyMatrix4(inverse.clone().multiply(mesh.matrixWorld));
    parts.push(geometry); meshes.push(mesh);
  });
  if (!parts.length) return;
  const count = parts.reduce((n, g) => n + g.attributes.position.count, 0);
  const indexCount = parts.reduce((n, g) => n + g.index.count, 0);
  const merged = new THREE.BufferGeometry(), indices = new Uint32Array(indexCount);
  for (const [name, size] of [['position', 3], ['normal', 3], ['uv', 2]]) {
    const values = new Float32Array(count * size); let offset = 0;
    for (const g of parts) { values.set(g.attributes[name].array, offset); offset += g.attributes[name].array.length; }
    merged.setAttribute(name, new THREE.BufferAttribute(values, size));
  }
  let vertex = 0, offset = 0;
  for (const g of parts) {
    for (const index of g.index.array) indices[offset++] = vertex + index;
    vertex += g.attributes.position.count; g.dispose();
  }
  merged.setIndex(new THREE.BufferAttribute(indices, 1));
  merged.computeBoundingSphere();
  for (const mesh of meshes) { mesh.removeFromParent(); mesh.geometry.dispose(); }
  world.add(new THREE.Mesh(merged, material));
}
